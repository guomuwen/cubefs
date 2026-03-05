// Copyright (C) 2020 Juicefs
// Modified work Copyright 2022 The CubeFS Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
// implied. See the License for the specific language governing
// permissions and limitations under the License.

package bcache

import (
	"bufio"
	"bytes"
	"container/list"
	"crypto/md5"
	"encoding/binary"
	"encoding/hex"
	"fmt"
	"hash/crc32"
	"io"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"syscall"
	"time"

	"github.com/cubefs/cubefs/util/bytespool"
	"github.com/cubefs/cubefs/util/errors"
	"github.com/cubefs/cubefs/util/exporter"
	"github.com/cubefs/cubefs/util/log"
	"github.com/cubefs/cubefs/util/stat"
)

const (
	PathListSeparator    = ";"
	CacheConfSeparator   = ":"
	SpaceCheckInterval   = 60 * time.Second
	TmpFileCheckInterval = 20 * 60 * time.Second
	FilePerm             = 0o644
	Basedir              = "blocks"
)

type ReadCloser interface {
	io.Reader
	io.ReaderAt
	io.Closer
}

type BcacheManager interface {
	cache(vol, key string, data []byte, direct bool)
	read(key string, offset uint64, len uint32) (io.ReadCloser, error)
	queryCachePath(key string, offset uint64, len uint32) (string, error)
	load(key string) (ReadCloser, error)
	erase(key string)
	stats() (int64, int64)
}

func newBcacheManager(conf *bcacheConfig) BcacheManager {
	log.LogInfof("init block cache: %s size:%d GB", conf.CacheDir, conf.BlockSize)
	if conf.CacheDir == "" {
		log.LogWarnf("no cache config,cacheDirs or size is empty!")
		return nil
	}
	// todo cachedir reg match
	cacheDirs := strings.Split(conf.CacheDir, PathListSeparator)
	if len(cacheDirs) == 0 {
		log.LogWarnf("no cache dir config!")
		return nil
	}

	dirSizeMap := make(map[string]int64, len(cacheDirs))
	for _, dir := range cacheDirs {
		result := strings.Split(dir, CacheConfSeparator)
		dirPath := result[0]
		cacheSize, err := strconv.Atoi(result[1])
		if dirPath == "" || err != nil {
			log.LogWarnf("cache dir config error!")
			return nil
		}
		dirSizeMap[dirPath] = int64(cacheSize)
		conf.CacheSize = conf.CacheSize + int64(cacheSize)
	}

	bm := &bcacheManager{
		bstore:         make([]*DiskStore, len(cacheDirs)),
		blockSize:      conf.BlockSize,
		pending:        make(chan waitFlush, 4096),
		cacheMetaCount: exporter.NewGaugeVec("bcache_meta_count", "", []string{"volName", "disk", "type"}),
		vol:            conf.Vol,
		encrypt:        conf.Encrypt,
	}
	for i := range bm.shards {
		bm.shards[i].keys = make(map[string]*list.Element)
		bm.shards[i].lrulist = list.New()
	}
	index := 0
	for cacheDir, cacheSize := range dirSizeMap {
		bm.checkEncryptMode(cacheDir, conf.Encrypt)
		disk := NewDiskStore(cacheDir, cacheSize, conf)
		bm.bstore[index] = disk
		go bm.reBuildCacheKeys(cacheDir, disk)
		index++
	}
	go bm.spaceManager()
	for i := 0; i < flushWorkerCount; i++ {
		go bm.flush()
	}
	return bm
}

type cacheItem struct {
	key  string
	size uint32
}

type keyPair struct {
	key string
	it  *cacheItem
}

// key vid_inode_offset
type waitFlush struct {
	Key  string
	Data []byte
}

const (
	flushWorkerCount = 8
	lruShardCount    = 64
)

type lruShard struct {
	sync.RWMutex
	keys    map[string]*list.Element
	lrulist *list.List
}

type bcacheManager struct {
	shards         [lruShardCount]lruShard
	bstore         []*DiskStore
	blockSize      uint32
	pending        chan waitFlush
	cacheMetaCount *exporter.GaugeVec
	vol            string
	encrypt        bool
}

func (bm *bcacheManager) getShard(key string) *lruShard {
	return &bm.shards[hashKey(key)%lruShardCount]
}

func encryptXOR(data []byte) {
	const xorWord = uint64(0x0F0F0F0F0F0F0F0F)
	n := len(data)
	// process 8 bytes at a time
	i := 0
	for ; i+8 <= n; i += 8 {
		v := binary.LittleEndian.Uint64(data[i:])
		binary.LittleEndian.PutUint64(data[i:], v^xorWord)
	}
	// process remaining bytes
	for ; i < n; i++ {
		data[i] ^= 0xF
	}
}

func (bm *bcacheManager) checkEncryptMode(cacheDir string, encrypt bool) {
	markerPath := filepath.Join(cacheDir, ".encrypt_mode")
	currentMode := strconv.FormatBool(encrypt)
	if f, err := os.Open(markerPath); err == nil {
		buf := make([]byte, 16)
		n, _ := f.Read(buf)
		f.Close()
		if n > 0 {
			oldMode := strings.TrimSpace(string(buf[:n]))
			if oldMode != currentMode {
				log.LogWarnf("encrypt mode changed from %s to %s, clearing cache dir %s", oldMode, currentMode, cacheDir)
				blocksDir := filepath.Join(cacheDir, Basedir)
				os.RemoveAll(blocksDir)
			}
		}
	}
	os.MkdirAll(cacheDir, os.FileMode(FilePerm))
	if f, err := os.OpenFile(markerPath, os.O_CREATE|os.O_WRONLY|os.O_TRUNC, os.FileMode(FilePerm)); err == nil {
		f.Write([]byte(currentMode))
		f.Close()
	}
}

func (bm *bcacheManager) queryCachePath(key string, offset uint64, len uint32) (path string, err error) {
	bgTime := stat.BeginStat()
	defer func() {
		stat.EndStat("GetCache:GetCachePath", err, bgTime, 1)
	}()

	shard := bm.getShard(key)
	shard.RLock()
	element, ok := shard.keys[key]
	shard.RUnlock()
	if ok {
		item := element.Value.(*cacheItem)
		path, err := bm.getCachePath(key)
		if err != nil {
			return "", err
		}
		shard.Lock()
		shard.lrulist.MoveToBack(element)
		shard.Unlock()
		log.LogDebugf("Cache item found. key=%v offset =%v,len=%v size=%v, path=%v", key, offset, len, item.size, path)
		return path, nil
	}
	log.LogDebugf("Cache item not found. key=%v offset =%v,len=%v", key, offset, len)
	return "", os.ErrNotExist
}

func (bm *bcacheManager) getCachePath(key string) (string, error) {
	if len(bm.bstore) == 0 {
		return "", errors.New("no cache dir")
	}
	cachePath := bm.selectDiskKv(key).getPath(key)
	return cachePath, nil
}

func (bm *bcacheManager) cache(vol, key string, data []byte, direct bool) {
	bgTime := stat.BeginStat()
	defer func() {
		stat.EndStat("Cache:Write", nil, bgTime, 1)
		stat.StatBandWidth("Cache", uint32(len(data)))
	}()
	log.LogDebugf("TRACE cache. key(%v)  len(%v) direct(%v)", key, len(data), direct)
	if direct {
		bm.cacheDirect(vol, key, data)
		return
	}
	select {
	case bm.pending <- waitFlush{Key: key, Data: data}:
	default:
		log.LogDebugf("pending chan is full,skip memory. key =%v,len=%v bytes", key, len(data))
		bm.cacheDirect(vol, key, data)
	}
}

func (bm *bcacheManager) cacheDirect(vol, key string, data []byte) {
	diskKv := bm.selectDiskKv(key)
	if diskKv.flushKey(vol, key, data, bm.encrypt) == nil {
		shard := bm.getShard(key)
		shard.Lock()
		item := &cacheItem{
			key:  key,
			size: uint32(len(data)),
		}
		element := shard.lrulist.PushBack(item)
		shard.keys[key] = element
		shard.Unlock()
	}
}

func (bm *bcacheManager) read(key string, offset uint64, len uint32) (io.ReadCloser, error) {
	var err error
	bgTime := stat.BeginStat()
	defer func() {
		stat.EndStat("GetCache:Read", err, bgTime, 1)
		if err == nil {
			stat.StatBandWidth("GetCache:Read", len)
		}
	}()
	metaBgTime := stat.BeginStat()
	shard := bm.getShard(key)
	shard.RLock()
	element, ok := shard.keys[key]
	shard.RUnlock()
	stat.EndStat("GetCache:Read:GetMeta", nil, metaBgTime, 1)
	log.LogDebugf("Trace read. ok =%v", ok)
	if ok {
		item := element.Value.(*cacheItem)
		f, err := bm.load(key)
		if os.IsNotExist(err) {
			shard.Lock()
			delete(shard.keys, key)
			shard.Unlock()
			d := bm.selectDiskKv(key)
			atomic.AddInt64(&d.usedSize, -int64(item.size))
			atomic.AddInt64(&d.usedCount, -1)
			return nil, os.ErrNotExist
		}
		if err != nil {
			return nil, err
		}
		defer f.Close()
		size := item.size
		log.LogDebugf("read. offset =%v,len=%v size=%v", offset, len, size)
		if uint32(offset)+len > size {
			len = size - uint32(offset)
		}
		dataBgTime := stat.BeginStat()
		buf := make([]byte, len)
		n, err := f.ReadAt(buf, int64(offset))
		stat.EndStat("GetCache:Read:ReadData", err, dataBgTime, 1)
		if err != nil {
			return nil, err
		} else {
			if bm.encrypt {
				encryptXOR(buf[:n])
			}
			return io.NopCloser(bytes.NewBuffer(buf[:n])), nil
		}
	} else {
		err = os.ErrNotExist
	}
	return nil, err
}

func (bm *bcacheManager) load(key string) (ReadCloser, error) {
	if len(bm.bstore) == 0 {
		return nil, errors.New("no cache dir")
	}
	f, err := bm.selectDiskKv(key).load(key)
	if err != nil {
		return nil, err
	}
	shard := bm.getShard(key)
	shard.Lock()
	if element, ok := shard.keys[key]; ok {
		shard.lrulist.MoveToBack(element)
	}
	shard.Unlock()
	return f, err
}

func (bm *bcacheManager) erase(key string) {
	if len(bm.bstore) == 0 {
		return
	}
	err := bm.selectDiskKv(key).remove(key)
	if err == nil {
		shard := bm.getShard(key)
		shard.Lock()
		if element, ok := shard.keys[key]; ok {
			shard.lrulist.Remove(element)
		}
		delete(shard.keys, key)
		shard.Unlock()
	}
}

func (bm *bcacheManager) stats() (int64, int64) {
	var usedCount, usedSize int64
	for _, item := range bm.bstore {
		usedSize += atomic.LoadInt64(&item.usedSize)
		usedCount += atomic.LoadInt64(&item.usedCount)
	}
	return usedCount, usedSize
}

func (bm *bcacheManager) selectDiskKv(key string) *DiskStore {
	return bm.bstore[hashKey(key)%uint32(len(bm.bstore))]
}

func (bm *bcacheManager) spaceManager() {
	ticker := time.NewTicker(SpaceCheckInterval)
	tmpTicker := time.NewTicker(TmpFileCheckInterval)

	defer func() {
		ticker.Stop()
		tmpTicker.Stop()
	}()
	for {
		select {
		case <-ticker.C:
			for _, store := range bm.bstore {
				useRatio, files := store.diskUsageRatio()
				log.LogDebugf("useRation(%v), files(%v)", useRatio, files)
				if 1-useRatio < store.freeLimit || files > int64(store.limit) {
					bm.freeSpace(store, 1-useRatio, files)
				}
				bm.cacheMetaCount.SetWithLabelValues(float64(useRatio), bm.vol, store.dir, "useRatio")
				bm.cacheMetaCount.SetWithLabelValues(float64(files), bm.vol, store.dir, "files")
			}
		case <-tmpTicker.C:
			for _, store := range bm.bstore {
				useRatio, files := store.diskUsageRatio()
				log.LogInfof("useRation(%v), files(%v)", useRatio, files)
				bm.deleteTmpFile(store)
			}
		}
	}
}

//lru cache
//func (bm *bcacheManager) freeSpace(index int, store *DiskStore, free float32, files int64) {
//	var decreaseSpace int64
//	var decreaseCnt int
//	storeCnt := uint32(len(bm.bstore))
//	bm.Lock()
//	defer bm.Unlock()
//	if free < store.freeLimit {
//		decreaseSpace = int64((store.freeLimit - free) * (float32(store.capacity)))
//	}
//	if files > int64(store.limit) {
//		decreaseCnt = int(files - int64(store.limit))
//	}
//	var lastKey string
//	var lastItem cacheItem
//	var cnt int
//	for key, value := range bm.bcacheKeys {
//		if int(hashKey(key)%storeCnt) == index {
//			if cnt == 0 || lastItem.atime > value.atime {
//				lastKey = key
//				lastItem = value
//			}
//			cnt++
//			if cnt > 1 {
//				store.remove(lastKey)
//				delete(bm.bcacheKeys, lastKey)
//				decreaseSpace -= int64(value.size)
//				decreaseCnt--
//				cnt = 0
//				log.LogDebugf("remove %s from cache, age: %d", lastKey, lastItem.atime)
//				if decreaseCnt <= 0 && decreaseSpace <= 0 {
//					break
//				}
//			}
//		}
//	}
//
//}

// lru
func (bm *bcacheManager) freeSpace(store *DiskStore, free float32, files int64) {
	var decreaseSpace int64
	var decreaseCnt int

	if free < store.freeLimit {
		decreaseSpace = int64((store.freeLimit - free) * (float32(store.capacity)))
	}
	if files > int64(store.limit) {
		decreaseCnt = int(files - int64(store.limit))
	}

	cnt := 0
	for decreaseCnt > 0 || decreaseSpace > 0 {
		if cnt > 500000 {
			break
		}
		evicted := false
		for i := range bm.shards {
			if decreaseCnt <= 0 && decreaseSpace <= 0 {
				break
			}
			shard := &bm.shards[i]
			shard.Lock()
			element := shard.lrulist.Front()
			if element == nil {
				shard.Unlock()
				continue
			}
			item := element.Value.(*cacheItem)
			if err := store.remove(item.key); err == nil {
				shard.lrulist.Remove(element)
				delete(shard.keys, item.key)
				decreaseSpace -= int64(item.size)
				decreaseCnt--
				cnt++
				evicted = true
				log.LogDebugf("remove %v from cache", item.key)
			}
			shard.Unlock()
		}
		if !evicted {
			break
		}
	}
}

func (bm *bcacheManager) reBuildCacheKeys(dir string, store *DiskStore) {
	if _, err := os.Stat(dir); err != nil {
		log.LogErrorf("cache dir %s is not exists", dir)
		return
	}
	log.LogDebugf("reBuildCacheKeys(%s)", dir)
	c := make(chan keyPair)
	keyPrefix := filepath.Join(dir, Basedir)
	go func() {
		filepath.Walk(dir, bm.walker(c, keyPrefix, true))
		close(c)
	}()

	for value := range c {
		shard := bm.getShard(value.key)
		shard.Lock()
		element := shard.lrulist.PushBack(value.it)
		shard.keys[value.key] = element
		shard.Unlock()
		log.LogDebugf("updateStat(%v)", value.it.size)
		store.updateStat(value.it.size)
	}
}

func (bm *bcacheManager) walker(c chan keyPair, prefix string, initial bool) filepath.WalkFunc {
	return func(path string, info os.FileInfo, err error) error {
		if err != nil {
			log.LogWarnf("walk path %v failed %v", path, err)
			return err
		}
		if info.IsDir() || !strings.HasPrefix(path, prefix) {
			return nil
		}
		if strings.HasSuffix(path, ".tmp") && (initial || checkoutTempFileOuttime(path)) {
			os.Remove(path)
			log.LogDebugf("Remove tmp file %v", path)
			return nil
		}
		_, key := filepath.Split(path)
		size := uint32(info.Size())
		pair := keyPair{
			key: key,
			it: &cacheItem{
				key:  key,
				size: size,
			},
		}
		c <- pair
		return nil
	}
}

func (bm *bcacheManager) flush() {
	for {
		pending := <-bm.pending
		diskKv := bm.selectDiskKv(pending.Key)
		log.LogDebugf("flush data,key(%v), dir(%v)", pending.Key, diskKv.dir)
		if diskKv.flushKey(bm.vol, pending.Key, pending.Data, bm.encrypt) == nil {
			shard := bm.getShard(pending.Key)
			shard.Lock()
			item := &cacheItem{
				key:  pending.Key,
				size: uint32(len(pending.Data)),
			}
			element := shard.lrulist.PushBack(item)
			shard.keys[pending.Key] = element
			shard.Unlock()
		}
	}
}

func hashKey(key string) uint32 {
	return crc32.ChecksumIEEE([]byte(key))
}

type DiskStore struct {
	sync.Mutex
	dir       string
	mode      uint32
	capacity  int64
	freeLimit float32
	limit     uint32
	usedSize  int64
	usedCount int64
}

func NewDiskStore(dir string, cacheSize int64, config *bcacheConfig) *DiskStore {
	if config.Mode == 0 {
		config.Mode = FilePerm
	}
	if config.FreeRatio <= 0 {
		config.FreeRatio = 0.15
	}

	if config.Limit <= 0 {
		config.Limit = 50000000
	}

	if config.Limit > 50000000 {
		config.Limit = 50000000
	}
	c := &DiskStore{
		dir:       dir,
		mode:      config.Mode,
		capacity:  cacheSize,
		freeLimit: config.FreeRatio,
		limit:     config.Limit,
	}
	log.LogDebugf("ignored method DiskStore.scrub at %p", c.scrub) // TODO: ignored
	c.checkBuildCacheDir(dir)
	return c
}

func (d *DiskStore) checkBuildCacheDir(dir string) {
	mode := os.FileMode(d.mode)
	if st, err := os.Stat(dir); os.IsNotExist(err) {
		if parent := filepath.Dir(dir); parent != dir {
			d.checkBuildCacheDir(parent)
		}
		os.Mkdir(dir, mode)
	} else if err != nil && st.Mode() != mode {
		os.Chmod(dir, mode)
	}
}

//func (d *DiskStore) flushKey(key string, data []byte) error {
//	var err error
//	bgTime := stat.BeginStat()
//	defer func() {
//		stat.EndStat("Cache:Write:FlushData", err, bgTime, 1)
//	}()
//	cachePath := d.buildCachePath(key, d.dir)
//	log.LogDebugf("TRACE BCacheService flushKey Enter. key(%v) cachePath(%v)", key, cachePath)
//	d.checkBuildCacheDir(filepath.Dir(cachePath))
//	tmp := cachePath + ".tmp"
//	f, err := os.OpenFile(tmp, os.O_CREATE|os.O_WRONLY|os.O_TRUNC, os.FileMode(d.mode))
//	defer os.Remove(tmp)
//	if err != nil {
//		log.LogErrorf("Create block tmp file:%s err:%s!", tmp, err)
//		return err
//	}
//	//encrypt
//	encryptXOR(data)
//	_, err = f.Write(data)
//	if err != nil {
//		f.Close()
//		log.LogErrorf("Write tmp failed: file %s err %s!", tmp, err)
//		return err
//	}
//	err = f.Close()
//	if err != nil {
//		log.LogErrorf("Close tmp failed: file:%s err:%s!", tmp, err)
//		return err
//	}
//	info, err := os.Stat(cachePath)
//	//if already cached
//	if !os.IsNotExist(err) {
//		atomic.AddInt64(&d.usedSize, -(info.Size()))
//		atomic.AddInt64(&d.usedCount, -1)
//		os.Remove(cachePath)
//	}
//	err = os.Rename(tmp, cachePath)
//	if err != nil {
//		log.LogErrorf("Rename block tmp file:%s err:%s!", tmp, err)
//		return err
//	}
//	atomic.AddInt64(&d.usedSize, int64(len(data)))
//	atomic.AddInt64(&d.usedCount, 1)
//	log.LogDebugf("TRACE BCacheService flushKey Exit. key(%v) cachePath(%v)", key, cachePath)
//	return nil
//
//}

func (d *DiskStore) flushKey(vol, key string, data []byte, encrypt bool) error {
	var err error
	bgTime := stat.BeginStat()
	metric := exporter.NewTPCnt("cacheToDisk")
	defer func() {
		stat.EndStat("Cache:Write:FlushData", err, bgTime, 1)
		metric.SetWithLabels(err, map[string]string{exporter.Vol: vol})
	}()
	cachePath := d.buildCachePath(key, d.dir)
	info, err := os.Stat(cachePath)
	// if already cached
	if err == nil && info.Size() > 0 {
		return nil
	}
	log.LogDebugf("TRACE BCacheService flushKey Enter. key(%v) cachePath(%v)", key, cachePath)
	d.checkBuildCacheDir(filepath.Dir(cachePath))
	tmp := cachePath + ".tmp"
	f, err := os.OpenFile(tmp, os.O_CREATE|os.O_WRONLY|os.O_TRUNC, os.FileMode(d.mode))
	defer os.Remove(tmp)
	if err != nil {
		log.LogWarnf("Create block tmp file:%s err:%s!", tmp, err)
		return err
	}
	if encrypt {
		encrypted := bytespool.Alloc(len(data))
		copy(encrypted, data)
		encryptXOR(encrypted)
		_, err = f.Write(encrypted)
		bytespool.Free(encrypted)
	} else {
		_, err = f.Write(data)
	}
	if err != nil {
		f.Close()
		log.LogErrorf("Write tmp failed: file %s err %s!", tmp, err)
		return err
	}
	err = f.Close()
	if err != nil {
		log.LogErrorf("Close tmp failed: file:%s err:%s!", tmp, err)
		return err
	}
	//info, err := os.Stat(cachePath)
	////if already cached
	//if !os.IsNotExist(err) {
	//	atomic.AddInt64(&d.usedSize, -(info.Size()))
	//	atomic.AddInt64(&d.usedCount, -1)
	//	os.Remove(cachePath)
	//}
	err = os.Rename(tmp, cachePath)
	if err != nil {
		log.LogErrorf("Rename block tmp file:%s err:%s!", tmp, err)
		return err
	}
	atomic.AddInt64(&d.usedSize, int64(len(data)))
	atomic.AddInt64(&d.usedCount, 1)
	log.LogDebugf("TRACE BCacheService flushKey Exit. key(%v) cachePath(%v)", key, cachePath)
	return nil
}

func (d *DiskStore) load(key string) (ReadCloser, error) {
	cachePath := d.buildCachePath(key, d.dir)
	log.LogDebugf("TRACE BCacheService load Enter. key(%v) cachePath(%v)", key, cachePath)
	//if _, err := os.Stat(cachePath); err != nil {
	//	return nil, errors.NewError(os.ErrNotExist)
	//}
	f, err := os.OpenFile(cachePath, os.O_RDONLY, os.FileMode(d.mode))
	log.LogDebugf("TRACE BCacheService load Exit. err(%v)", err)
	return f, err
}

func (d *DiskStore) remove(key string) (err error) {
	var size int64
	cachePath := d.buildCachePath(key, d.dir)
	log.LogDebugf("remove. cachePath(%v)", cachePath)
	if info, err := os.Stat(cachePath); err == nil {
		size = info.Size()
		if err = os.Remove(cachePath); err == nil {
			atomic.AddInt64(&d.usedSize, -size)
			atomic.AddInt64(&d.usedCount, -1)
		}
	}
	return err
}

func (d *DiskStore) buildCachePath(key string, dir string) string {
	inodeId, err := strconv.ParseInt(strings.Split(key, "_")[1], 10, 64)
	if err != nil {
		return fmt.Sprintf("%s/blocks/%d/%d/%s", dir, hashKey(key)&0xFFF%512, hashKey(key)%512, key)
	}
	return fmt.Sprintf("%s/blocks/%d/%d/%s", dir, hashKey(key)&0xFFF%512, inodeId%512, key)
}

func (d *DiskStore) diskUsageRatio() (float32, int64) {
	log.LogDebugf("usedSize(%v), usedCount(%v)", atomic.LoadInt64(&d.usedSize), atomic.LoadInt64(&d.usedCount))
	if atomic.LoadInt64(&d.usedSize) < 0 || atomic.LoadInt64(&d.usedCount) < 0 {
		return 0, 0
	}
	return float32(atomic.LoadInt64(&d.usedSize)) / float32(d.capacity), atomic.LoadInt64(&d.usedCount)
}

func (d *DiskStore) scrub(key string, md5Sum string) error {
	defer func() {
		if r := recover(); r != nil {
			return
		}
	}()
	cachePath := d.buildCachePath(key, d.dir)
	f, err := os.Open(cachePath)
	if err != nil {
		return err
	}
	defer f.Close()
	r := bufio.NewReader(f)
	h := md5.New()
	_, err = io.Copy(h, r)
	if err != nil {
		return err
	}
	if md5Sum != hex.EncodeToString(h.Sum(nil)) {
		return errors.New("scrub error")
	}
	return nil
}

func (d *DiskStore) updateStat(size uint32) {
	atomic.AddInt64(&d.usedSize, int64(size))
	atomic.AddInt64(&d.usedCount, 1)
}

func (d *DiskStore) getPath(key string) string {
	cachePath := d.buildCachePath(key, d.dir)
	return cachePath
}

func (bm *bcacheManager) deleteTmpFile(store *DiskStore) {
	if _, err := os.Stat(store.dir); err != nil {
		log.LogErrorf("cache dir %s is not exists", store.dir)
		return
	}
	log.LogDebugf("clear tmp files in %v", store.dir)
	c := make(chan keyPair)
	keyPrefix := filepath.Join(store.dir, Basedir)
	log.LogDebugf("keyPrefix %v", keyPrefix)
	go func() {
		filepath.Walk(store.dir, bm.walker(c, keyPrefix, false))
		close(c)
	}()
	// consume chan
	for range c {
	}

	log.LogDebugf("clear tmp files end%v", store.dir)
}

func checkoutTempFileOuttime(file string) bool {
	finfo, err := os.Stat(file)
	if err != nil {
		return false
	}
	stat_t := finfo.Sys().(*syscall.Stat_t)
	now := time.Now()
	return now.Sub(timespecToTime(stat_t.Ctim)).Seconds() > 60*60 // 1 hour
}

func timespecToTime(ts syscall.Timespec) time.Time {
	return time.Unix(int64(ts.Sec), int64(ts.Nsec))
}
