// Copyright 2022 The CubeFS Authors.
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
	"container/list"
	"hash/crc32"
	"os"
	"strings"
	"sync"
	"time"

	"github.com/cubefs/cubefs/blobstore/util/bytespool"
	"github.com/cubefs/cubefs/proto"
	"github.com/cubefs/cubefs/util/errors"
	"github.com/cubefs/cubefs/util/exporter"
	"github.com/cubefs/cubefs/util/log"
	"github.com/cubefs/cubefs/util/stat"
)

const (
	_ int = iota
	statusOK
	statusNoent
	statusError

	pathCacheShards = 64
)

type pathCacheShard struct {
	sync.RWMutex
	items map[string]string
}

type pathCache struct {
	shards [pathCacheShards]pathCacheShard
}

func newPathCache() *pathCache {
	pc := &pathCache{}
	for i := range pc.shards {
		pc.shards[i].items = make(map[string]string)
	}
	return pc
}

func (pc *pathCache) getShard(key string) *pathCacheShard {
	return &pc.shards[crc32.ChecksumIEEE([]byte(key))%pathCacheShards]
}

func (pc *pathCache) get(key string) (string, bool) {
	shard := pc.getShard(key)
	shard.RLock()
	path, ok := shard.items[key]
	shard.RUnlock()
	return path, ok
}

func (pc *pathCache) put(key, path string) {
	shard := pc.getShard(key)
	shard.Lock()
	shard.items[key] = path
	shard.Unlock()
}

func (pc *pathCache) remove(key string) {
	shard := pc.getShard(key)
	shard.Lock()
	delete(shard.items, key)
	shard.Unlock()
}

const (
	fdCacheShards      = 32
	fdCacheMaxPerShard = 8192 // 32 shards × 8192 = ~262K total FDs, balances hit rate vs kernel overhead
)

// fdCacheEntry holds a file descriptor and its LRU linked list element.
type fdCacheEntry struct {
	f    *os.File
	path string
	elem *list.Element
}

type fdCacheShard struct {
	sync.RWMutex
	items map[string]*fdCacheEntry
	lru   *list.List // front = most recently used, back = least recently used
}

type fdCache struct {
	shards [fdCacheShards]fdCacheShard
}

func newFdCache() *fdCache {
	fc := &fdCache{}
	for i := range fc.shards {
		fc.shards[i].items = make(map[string]*fdCacheEntry)
		fc.shards[i].lru = list.New()
	}
	return fc
}

func (fc *fdCache) getShard(key string) *fdCacheShard {
	return &fc.shards[crc32.ChecksumIEEE([]byte(key))%fdCacheShards]
}

func (fc *fdCache) getOrOpen(cachePath string) (*os.File, error) {
	shard := fc.getShard(cachePath)

	// Fast path: read lock only, skip LRU promotion to avoid write lock contention.
	// With 32K capacity per shard (covering full working set), eviction is rare,
	// so LRU accuracy is not critical on the hot path.
	shard.RLock()
	if entry, ok := shard.items[cachePath]; ok {
		shard.RUnlock()
		return entry.f, nil
	}
	shard.RUnlock()

	f, err := os.Open(cachePath)
	if err != nil {
		return nil, err
	}

	shard.Lock()
	// Double-check after acquiring write lock
	if existing, ok := shard.items[cachePath]; ok {
		shard.Unlock()
		f.Close()
		return existing.f, nil
	}
	// Evict LRU entries if at capacity
	for shard.lru.Len() >= fdCacheMaxPerShard {
		tail := shard.lru.Back()
		if tail == nil {
			break
		}
		evictPath := tail.Value.(string)
		shard.lru.Remove(tail)
		if evictEntry, ok := shard.items[evictPath]; ok {
			evictEntry.f.Close()
			delete(shard.items, evictPath)
		}
	}
	elem := shard.lru.PushFront(cachePath)
	shard.items[cachePath] = &fdCacheEntry{f: f, path: cachePath, elem: elem}
	shard.Unlock()
	return f, nil
}

func (fc *fdCache) evict(cachePath string) {
	shard := fc.getShard(cachePath)
	shard.Lock()
	if entry, ok := shard.items[cachePath]; ok {
		entry.f.Close()
		if entry.elem != nil {
			shard.lru.Remove(entry.elem)
		}
		delete(shard.items, cachePath)
	}
	shard.Unlock()
}

type BcacheClient struct {
	connPool  *ConnPool
	pathCache *pathCache
	fdCache   *fdCache
	encrypt   bool
}

var (
	once   sync.Once
	client *BcacheClient
)

func NewBcacheClient() *BcacheClient {
	return NewBcacheClientWithEncrypt(true)
}

func NewBcacheClientWithEncrypt(encrypt bool) *BcacheClient {
	once.Do(func() {
		expireTime := int64(time.Second * ConnectExpireTime)
		cp := NewConnPool(UnixSocketPath, 20, 200, expireTime)
		client = &BcacheClient{
			connPool:  cp,
			pathCache: newPathCache(),
			fdCache:   newFdCache(),
			encrypt:   encrypt,
		}
	})
	return client
}

func (c *BcacheClient) Get(vol, key string, buf []byte, offset uint64, size uint32) (int, error) {
	var err error
	bgTime := stat.BeginStat()
	defer func() {
		stat.EndStat("bcache-get", err, bgTime, 1)
	}()

	if cachePath, ok := c.pathCache.get(key); ok {
		n, readErr := c.readCacheFile(cachePath, key, buf, offset, size)
		if readErr == nil {
			stat.EndStat("bcache-get-fast", nil, bgTime, 1)
			return n, nil
		}
		if os.IsNotExist(readErr) {
			c.pathCache.remove(key)
		}
	}

	cachePath, err := c.getPathViaIPC(vol, key, offset, size)
	if err != nil {
		return 0, err
	}

	c.pathCache.put(key, cachePath)

	readBgTime := stat.BeginStat()
	readCacheMetric := exporter.NewTPCnt("bcache-read-cachefile")
	n, err := c.readCacheFile(cachePath, key, buf, offset, size)
	readCacheMetric.SetWithLabels(err, map[string]string{exporter.Vol: vol})
	stat.EndStat("bcache-get-read", err, readBgTime, 1)
	return n, err
}

func (c *BcacheClient) readCacheFile(cachePath, key string, buf []byte, offset uint64, size uint32) (int, error) {
	subs := strings.Split(cachePath, "/")
	if subs[len(subs)-1] != key {
		return 0, errors.NewErrorf("cacheKey(%v) cache path is not legal: %v", key, cachePath)
	}
	f, err := c.fdCache.getOrOpen(cachePath)
	if err != nil {
		return 0, err
	}
	n, err := f.ReadAt(buf, int64(offset))
	if err != nil {
		c.fdCache.evict(cachePath)
		return 0, err
	}
	if n != int(size) {
		return 0, errors.NewErrorf("BcacheClient GET() size mismatch: expect(%v) got(%v)", size, n)
	}
	if c.encrypt {
		encryptXOR(buf[:n])
	}
	return n, nil
}

func (c *BcacheClient) getPathViaIPC(vol, key string, offset uint64, size uint32) (string, error) {
	req := &GetCacheRequest{CacheKey: key, Offset: offset, Size: size}
	packet := NewBlockCachePacket()
	packet.Opcode = OpBlockCacheGet
	data, err := req.Marshal()
	if err != nil {
		return "", err
	}
	defer bytespool.Free(data)
	packet.Data = data
	packet.Size = uint32(len(packet.Data))

	conn, err := c.connPool.Get()
	if err != nil {
		return "", err
	}
	defer c.connPool.Put(conn)

	getCachePathMetric := exporter.NewTPCnt("bcache-get-cachepath")
	if err = packet.WriteToConn(*conn); err != nil {
		getCachePathMetric.SetWithLabels(err, map[string]string{exporter.Vol: vol})
		return "", errors.NewErrorf("Failed to write to conn, req(%v) err(%v)", req.CacheKey, err)
	}
	if err = packet.ReadFromConn(*conn, 1); err != nil {
		getCachePathMetric.SetWithLabels(err, map[string]string{exporter.Vol: vol})
		return "", errors.NewErrorf("Failed to read from conn, req(%v), err(%v)", req.CacheKey, err)
	}
	if parseStatus(packet.ResultCode) != statusOK {
		err = errors.New(packet.GetResultMsg())
		getCachePathMetric.SetWithLabels(err, map[string]string{exporter.Vol: vol})
		return "", err
	}

	resp := new(GetCachePathResponse)
	if err = resp.UnmarshalValue(packet.Data); err != nil {
		getCachePathMetric.SetWithLabels(err, map[string]string{exporter.Vol: vol})
		return "", err
	}
	getCachePathMetric.SetWithLabels(nil, map[string]string{exporter.Vol: vol})
	return resp.CachePath, nil
}

func (c *BcacheClient) Put(vol, key string, buf []byte) error {
	var err error
	bgTime := stat.BeginStat()
	cacheFileMetric := exporter.NewTPCnt("bcache-put-cachefile")
	defer func() {
		stat.EndStat("bcache-put", err, bgTime, 1)
		cacheFileMetric.SetWithLabels(err, map[string]string{exporter.Vol: vol})
	}()

	req := &PutCacheRequest{
		CacheKey: key,
		Data:     buf,
		VolName:  vol,
	}
	packet := NewBlockCachePacket()
	packet.Opcode = OpBlockCachePut
	data, err := req.Marshal()
	defer func() {
		bytespool.Free(data)
	}()
	if err != nil {
		log.LogDebugf("put block cache: req(%v) err(%v)", req.CacheKey, err)
		return err
	}
	packet.Data = data
	packet.Size = uint32(len(packet.Data))
	conn, err := c.connPool.Get()
	if err != nil {
		log.LogDebugf("put block cache: get Conn failed, req(%v) err(%v)", req.CacheKey, err)
		return err
	}
	defer func() {
		c.connPool.Put(conn)
	}()

	err = packet.WriteToConn(*conn)
	if err != nil {
		log.LogDebugf("Failed to write to conn, req(%v) err(%v)", req.CacheKey, err)
		return errors.NewErrorf("Failed to write to conn, req(%v) err(%v)", req.CacheKey, err)
	}

	err = packet.ReadFromConn(*conn, proto.NoReadDeadlineTime)
	if err != nil {
		log.LogDebugf("Failed to read from conn, req(%v), err(%v)", req.CacheKey, err)
		return errors.NewErrorf("Failed to read from conn, req(%v), err(%v)", req.CacheKey, err)
	}
	status := parseStatus(packet.ResultCode)
	if status != statusOK {
		err = errors.New(packet.GetResultMsg())
		log.LogDebugf("put block cache: req(%v) err(%v) result(%v)", req.CacheKey, err, packet.GetResultMsg())
		return err
	}

	return err
}

func (c *BcacheClient) Evict(key string) error {
	req := &DelCacheRequest{CacheKey: key}
	packet := NewBlockCachePacket()
	packet.Opcode = OpBlockCacheDel
	data, err := req.Marshal()
	if err != nil {
		log.LogDebugf("del block cache: req(%v) err(%v)", req.CacheKey, err)
		return err
	}
	defer func() {
		bytespool.Free(data)
	}()
	packet.Data = data
	packet.Size = uint32(len(packet.Data))
	conn, err := c.connPool.Get()
	if err != nil {
		log.LogDebugf("del block cache: get Conn failed, req(%v) err(%v)", req.CacheKey, err)
		return err
	}
	defer func() {
		c.connPool.Put(conn)
	}()

	err = packet.WriteToConn(*conn)
	if err != nil {
		return err
	}

	err = packet.ReadFromConn(*conn, proto.NoReadDeadlineTime)
	if err != nil {
		return err
	}
	status := parseStatus(packet.ResultCode)
	if status != statusOK {
		err = errors.New(packet.GetResultMsg())
		log.LogErrorf("del block cache: req(%v) err(%v) result(%v)", req.CacheKey, err, packet.GetResultMsg())
		return err
	}
	log.LogDebugf("del block cache success: req(%v)", req.CacheKey)
	return nil
}

func parseStatus(result uint8) (status int) {
	switch result {
	case proto.OpOk:
		status = statusOK
	case proto.OpNotExistErr:
		status = statusNoent
	default:
		status = statusError
	}
	return
}
