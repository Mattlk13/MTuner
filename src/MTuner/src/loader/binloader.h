//--------------------------------------------------------------------------//
/// Copyright 2026 Milos Tosic. All Rights Reserved.                       ///
/// License: http://www.opensource.org/licenses/BSD-2-Clause               ///
//--------------------------------------------------------------------------//

#ifndef RTM_MTUNER_BINLOADER_H
#define RTM_MTUNER_BINLOADER_H

#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>

namespace rtm {

// Streaming reader for capture files. For compressed captures a background producer thread
// reads and LZ4-decompresses chunks ahead into a bounded queue while the calling (consumer)
// thread parses, overlapping file I/O and decompression with parsing. This is cross-platform
// (std::thread) so it speeds up loading on every host, including Windows. Uncompressed
// captures are read directly with no helper thread.
class BinLoader
{
	struct Chunk
	{
		uint8_t*	m_data;				// decompressed bytes (owned, freed by the consumer)
		int32_t		m_size;				// decompressed byte count
		uint32_t	m_compressedSize;	// on-disk size of the chunk (header + payload), for progress
		bool		m_eof;				// sentinel: no more chunks
		bool		m_error;			// sentinel: read/decompress failure
	};

	uint8_t*	m_data;					// current decompressed chunk being consumed
	int32_t		m_dataAvailable;		// valid bytes in m_data
	int32_t		m_dataPos;				// consumer read offset into m_data
	uint64_t	m_bytesRead;			// decompressed bytes consumed from prior chunks
	uint64_t	m_consumedCompressed;	// on-disk bytes of fully-consumed chunks (progress)
	uint32_t	m_currentCompressed;	// on-disk size of the current chunk
	FILE*		m_file;
	bool		m_compressed;
	bool		m_eofReached;			// consumer has seen the terminal sentinel

	// producer/consumer plumbing (compressed captures only)
	std::thread					m_producer;
	std::mutex					m_mutex;
	std::condition_variable		m_cvNotFull;
	std::condition_variable		m_cvNotEmpty;
	std::deque<Chunk>			m_queue;
	bool						m_stop;			// consumer asked the producer to stop
	size_t						m_maxQueued;	// queue capacity (chunks in flight)

public:
	BinLoader(FILE* _file, bool _compressed);
	~BinLoader();

	// Stops and joins the producer thread. Must be called (or the destructor must run)
	// before the underlying FILE* is closed. Idempotent.
	void stop();

	bool eof();
	uint64_t tell();
	uint64_t fileTell();
	int read(void* _ptr, size_t _size);

	template <typename T>
	int readVar(T& _var)
	{
		return read(&_var, sizeof(T));
	}

private:
	bool loadChunk();			// consumer: swap in the next decompressed chunk
	void producerThread();		// producer: read + decompress chunks into the queue
};

} // namespace rtm

#endif // RTM_MTUNER_BINLOADER_H
