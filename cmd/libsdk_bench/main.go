package main

import (
	"flag"
	"fmt"
	"math/rand"
	"os"
	"path"
	"strings"
	"sync"
	"sync/atomic"
	"syscall"
	"time"

	"github.com/cubefs/cubefs/client/gosdk"
)

var (
	masterAddr = flag.String("master", "", "Master addresses (required, comma-separated)")
	volName    = flag.String("vol", "", "Volume name (required)")
	accessKey  = flag.String("ak", "", "Access key (required)")
	secretKey  = flag.String("sk", "", "Secret key (required)")
	dataDir    = flag.String("dir", "", "Data directory path relative to volume root (required)")
	workers    = flag.Int("workers", 128, "Number of concurrent goroutines")
	fileSize   = flag.Int("fileSize", 102400, "Bytes to read per file")
	maxFiles   = flag.Int("maxFiles", 0, "Max files to read (0 = all)")
	epochs     = flag.Int("epochs", 5, "Number of read epochs")
	shuffle    = flag.Bool("shuffle", true, "Shuffle file list before each epoch")
	enableBC      = flag.Bool("bcache", true, "Enable bcache acceleration")
	bcacheEncrypt = flag.Bool("bcacheEncrypt", false, "Enable bcache encryption (must match bcache-server cacheEncrypt)")
	bcacheDirs    = flag.String("bcacheDirs", "", "Semicolon-separated bcache dirs for IPC-bypass (e.g. /bcache0;/bcache1)")
	logDir     = flag.String("logDir", "/cfs/libsdk/log", "Log directory")
	logLevel   = flag.String("logLevel", "error", "Log level")
)

// stats counters (atomic)
var (
	completedFiles int64
	completedBytes int64
	errorCount     int64
)

func main() {
	flag.Parse()

	// Validate required parameters
	missing := []string{}
	if *masterAddr == "" {
		missing = append(missing, "-master")
	}
	if *volName == "" {
		missing = append(missing, "-vol")
	}
	if *accessKey == "" {
		missing = append(missing, "-ak")
	}
	if *secretKey == "" {
		missing = append(missing, "-sk")
	}
	if *dataDir == "" {
		missing = append(missing, "-dir")
	}
	if len(missing) > 0 {
		fmt.Fprintf(os.Stderr, "Error: missing required parameters: %s\n\n", strings.Join(missing, ", "))
		flag.Usage()
		os.Exit(1)
	}

	// Ensure log directory exists
	os.MkdirAll(*logDir, 0755)

	// Initialize gosdk client
	fmt.Printf("[Init] Connecting to CubeFS cluster...\n")
	fmt.Printf("[Init]   Master: %s\n", *masterAddr)
	fmt.Printf("[Init]   Volume: %s\n", *volName)
	fmt.Printf("[Init]   Bcache: %v\n", *enableBC)

	cfg := gosdk.Config{
		VolName:       *volName,
		MasterAddr:    *masterAddr,
		AccessKey:     *accessKey,
		SecretKey:     *secretKey,
		EnableBcache:  *enableBC,
		BcacheEncrypt: *bcacheEncrypt,
		BcacheDirs:    *bcacheDirs,
		NearRead:      true,
		LogDir:        *logDir,
		LogLevel:      *logLevel,
	}

	client := gosdk.New(cfg)
	if err := client.Start(); err != nil {
		fmt.Fprintf(os.Stderr, "[Init] Failed to start client: %v\n", err)
		os.Exit(1)
	}
	defer client.Close()
	fmt.Printf("[Init] Client initialized successfully\n")

	// Build file list
	fmt.Printf("[FileList] Reading directory: %s\n", *dataDir)
	startList := time.Now()
	fileList, err := listFiles(client, *dataDir, *maxFiles)
	if err != nil {
		fmt.Fprintf(os.Stderr, "[FileList] Failed to list files: %v\n", err)
		os.Exit(1)
	}
	listDuration := time.Since(startList)
	fmt.Printf("[FileList] Found %d files in %v\n", len(fileList), listDuration)

	if len(fileList) == 0 {
		fmt.Fprintf(os.Stderr, "[FileList] No files found in %s\n", *dataDir)
		os.Exit(1)
	}

	// Run benchmark
	fmt.Printf("\n[Benchmark] Starting: %d workers, %d files, %d epochs, fileSize=%d\n",
		*workers, len(fileList), *epochs, *fileSize)
	fmt.Printf("[Benchmark] FUSE baseline: 10260 IOPS (single SSD)\n\n")

	type epochResult struct {
		epoch    int
		iops     float64
		bwMBs    float64
		duration time.Duration
		files    int64
		errors   int64
	}
	results := make([]epochResult, 0, *epochs)

	for ep := 1; ep <= *epochs; ep++ {
		if *shuffle {
			rand.Shuffle(len(fileList), func(i, j int) {
				fileList[i], fileList[j] = fileList[j], fileList[i]
			})
		}

		// Reset counters
		atomic.StoreInt64(&completedFiles, 0)
		atomic.StoreInt64(&completedBytes, 0)
		atomic.StoreInt64(&errorCount, 0)

		// Start stats reporter
		stopStats := make(chan struct{})
		go statsReporter(ep, len(fileList), stopStats)

		// Distribute files to workers
		epochStart := time.Now()
		var wg sync.WaitGroup
		numWorkers := *workers
		if numWorkers > len(fileList) {
			numWorkers = len(fileList)
		}

		chunkSize := len(fileList) / numWorkers
		remainder := len(fileList) % numWorkers

		offset := 0
		for w := 0; w < numWorkers; w++ {
			size := chunkSize
			if w < remainder {
				size++
			}
			workerFiles := fileList[offset : offset+size]
			offset += size

			wg.Add(1)
			go func(files []string) {
				defer wg.Done()
				readWorker(client, files, *fileSize)
			}(workerFiles)
		}

		wg.Wait()
		epochDuration := time.Since(epochStart)
		close(stopStats)

		// Collect results
		files := atomic.LoadInt64(&completedFiles)
		bytes := atomic.LoadInt64(&completedBytes)
		errs := atomic.LoadInt64(&errorCount)
		iops := float64(files) / epochDuration.Seconds()
		bw := float64(bytes) / epochDuration.Seconds() / 1024 / 1024

		result := epochResult{
			epoch:    ep,
			iops:     iops,
			bwMBs:    bw,
			duration: epochDuration,
			files:    files,
			errors:   errs,
		}
		results = append(results, result)

		fmt.Printf("[Epoch %d] Summary: IOPS=%.0f  BW=%.1fMB/s  Duration=%.1fs  Files=%d  Errors=%d\n\n",
			ep, iops, bw, epochDuration.Seconds(), files, errs)
	}

	// Final summary
	fmt.Printf("==================== Final Summary ====================\n")
	fmt.Printf("%-8s %10s %12s %10s %10s %8s\n", "Epoch", "IOPS", "BW(MB/s)", "Duration", "Files", "Errors")
	fmt.Printf("%-8s %10s %12s %10s %10s %8s\n", "-----", "----", "--------", "--------", "-----", "------")

	var totalIOPS, totalBW float64
	for _, r := range results {
		fmt.Printf("%-8d %10.0f %12.1f %9.1fs %10d %8d\n",
			r.epoch, r.iops, r.bwMBs, r.duration.Seconds(), r.files, r.errors)
		totalIOPS += r.iops
		totalBW += r.bwMBs
	}

	avgIOPS := totalIOPS / float64(len(results))
	avgBW := totalBW / float64(len(results))
	fmt.Printf("%-8s %10.0f %12.1f\n", "Avg", avgIOPS, avgBW)
	fmt.Printf("========================================================\n")
	fmt.Printf("vs FUSE baseline (10260 IOPS): %.1fx\n", avgIOPS/10260)
}

func listFiles(client *gosdk.Client, dir string, max int) ([]string, error) {
	var files []string
	var walkDir func(string) error
	walkDir = func(d string) error {
		dirFile, err := client.OpenFile(d, syscall.O_RDONLY, 0)
		if err != nil {
			return fmt.Errorf("open dir %s: %v", d, err)
		}
		defer dirFile.CloseFile()

		batchSize := 10000
		for {
			entries, err := dirFile.Readdir(batchSize)
			if err != nil {
				return fmt.Errorf("readdir %s: %v", d, err)
			}
			if len(entries) == 0 {
				break
			}
			for _, e := range entries {
				if max > 0 && len(files) >= max {
					return nil
				}
				fullPath := path.Join(d, e.Name)
				if e.DType == syscall.DT_REG {
					files = append(files, fullPath)
				} else if e.DType == syscall.DT_DIR {
					if err := walkDir(fullPath); err != nil {
						return err
					}
				}
			}
			if len(entries) < batchSize {
				break
			}
		}
		return nil
	}
	if err := walkDir(dir); err != nil {
		return nil, err
	}
	if max > 0 && len(files) > max {
		files = files[:max]
	}
	return files, nil
}

func readWorker(client *gosdk.Client, files []string, readSize int) {
	buf := make([]byte, readSize)
	for _, filePath := range files {
		f, err := client.OpenFile(filePath, syscall.O_RDONLY, 0)
		if err != nil {
			atomic.AddInt64(&errorCount, 1)
			continue
		}
		n, err := f.ReadFile(buf, 0)
		if err != nil {
			atomic.AddInt64(&errorCount, 1)
			f.CloseFile()
			continue
		}
		f.CloseFile()

		atomic.AddInt64(&completedFiles, 1)
		atomic.AddInt64(&completedBytes, int64(n))
	}
}

func statsReporter(epoch int, totalFiles int, stop chan struct{}) {
	ticker := time.NewTicker(1 * time.Second)
	defer ticker.Stop()

	var lastFiles, lastBytes int64
	for {
		select {
		case <-stop:
			return
		case <-ticker.C:
			curFiles := atomic.LoadInt64(&completedFiles)
			curBytes := atomic.LoadInt64(&completedBytes)
			curErrors := atomic.LoadInt64(&errorCount)

			deltaFiles := curFiles - lastFiles
			deltaBytes := curBytes - lastBytes
			lastFiles = curFiles
			lastBytes = curBytes

			bw := float64(deltaBytes) / 1024 / 1024

			fmt.Printf("[Epoch %d] IOPS=%d  BW=%.1fMB/s  Files=%d/%d  Errors=%d\n",
				epoch, deltaFiles, bw, curFiles, totalFiles, curErrors)
		}
	}
}
