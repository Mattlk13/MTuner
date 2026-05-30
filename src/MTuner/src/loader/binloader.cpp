//--------------------------------------------------------------------------//
/// Copyright 2026 Milos Tosic. All Rights Reserved.                       ///
/// License: http://www.opensource.org/licenses/BSD-2-Clause               ///
//--------------------------------------------------------------------------//

#include <MTuner_pch.h>
#include <MTuner/src/loader/binloader.h>
#include <rmem/src/rmem_hook.h>
#include <rbase/inc/endianswap.h>

#define LZ5_DISABLE_DEPRECATE_WARNINGS
#include <rmem/3rd/lz4/lz4.c>

#if RTM_COMPILER_MSVC
#pragma intrinsic (memcpy)
#endif // RTM_COMPILER_MSVC

namespace rtm {

BinLoader::BinLoader(FILE* _file, bool _compressed) :
	m_file(_file)
{
	m_compressed			= _compressed;
	m_data					= 0;
	m_dataAvailable			= 0;
	m_dataPos				= 0;
	m_bytesRead				= 0;
	m_consumedCompressed	= 0;
	m_currentCompressed		= 0;
	m_eofReached			= false;
	m_stop					= false;
	m_maxQueued				= 16;	// up to 16 decompressed chunks (<= 16 * BufferSize) in flight

	if (m_compressed)
	{
		m_producer = std::thread(&BinLoader::producerThread, this);
		loadChunk();				// prime the first chunk (blocks until the producer delivers it)
	}
}

BinLoader::~BinLoader()
{
	stop();

	if (m_data)
		delete[] m_data;

	// drain any decompressed chunks still queued
	for (size_t i=0; i<m_queue.size(); ++i)
		if (m_queue[i].m_data)
			delete[] m_queue[i].m_data;
}

void BinLoader::stop()
{
	if (!m_producer.joinable())
		return;

	{
		std::unique_lock<std::mutex> lock(m_mutex);
		m_stop = true;
	}
	m_cvNotFull.notify_all();
	m_cvNotEmpty.notify_all();
	m_producer.join();
}

void BinLoader::producerThread()
{
	// Reused compressed-read buffer (touched only by the producer).
	int32_t  srcCap = rmem::MemoryHook::BufferSize;
	uint8_t* src	= new uint8_t[srcCap];

	for (;;)
	{
		{
			std::unique_lock<std::mutex> lock(m_mutex);
			if (m_stop)
				break;
		}

		Chunk chunk;
		chunk.m_data			= 0;
		chunk.m_size			= 0;
		chunk.m_compressedSize	= 0;
		chunk.m_eof				= false;
		chunk.m_error			= false;

		uint32_t sig = 0, size = 0;
		size_t e = fread(&sig, sizeof(uint32_t), 1, m_file);
		if (e != 1)
		{
			chunk.m_eof = true;		// normal end of stream
		}
		else if (!((sig == 0x23234646) || sig == endianSwap(uint32_t(0x23234646))))
		{
			chunk.m_error = true;
		}
		else
		{
			e = fread(&size, sizeof(uint32_t), 1, m_file);
			if (e == 0)
				chunk.m_error = true;
			else
			{
				if (sig == endianSwap(uint32_t(0x23234646)))
					size = endianSwap(size);

				if ((int32_t)size > srcCap)
				{
					delete[] src;
					srcCap	= (int32_t)size;
					src		= new uint8_t[srcCap];
				}

				e = fread(src, 1, size, m_file);
				if (e != size)
					chunk.m_error = true;
				else
				{
					// A chunk is compressed from at most rmem's buffer, so the decompressed
					// payload never exceeds BufferSize. We still grow defensively if needed.
					int32_t  dstCap = rmem::MemoryHook::BufferSize;
					uint8_t* dst	= new uint8_t[dstCap];
					int32_t  avail	= LZ4_decompress_safe((const char*)src, (char*)dst, size, dstCap);
					while (avail < 0)
					{
						delete[] dst;
						dstCap	*= 2;
						dst		 = new uint8_t[dstCap];
						avail	 = LZ4_decompress_safe((const char*)src, (char*)dst, size, dstCap);
					}

					chunk.m_data			= dst;
					chunk.m_size			= avail;
					chunk.m_compressedSize	= size + 2 * (uint32_t)sizeof(uint32_t);	// payload + sig + size header
				}
			}
		}

		const bool terminal = chunk.m_eof || chunk.m_error;

		{
			std::unique_lock<std::mutex> lock(m_mutex);
			m_cvNotFull.wait(lock, [this]{ return (m_queue.size() < m_maxQueued) || m_stop; });
			if (m_stop)
			{
				lock.unlock();
				if (chunk.m_data)
					delete[] chunk.m_data;
				break;
			}
			m_queue.push_back(chunk);
			m_cvNotEmpty.notify_one();
		}

		if (terminal)
			break;					// the sentinel has been delivered; nothing more to read
	}

	delete[] src;
}

bool BinLoader::loadChunk()
{
	// The producer delivers exactly one terminal sentinel and then exits, so once we have seen
	// it there is nothing left to wait for - return immediately to avoid blocking forever on an
	// empty queue with no producer.
	if (m_eofReached)
		return false;

	Chunk chunk;
	{
		std::unique_lock<std::mutex> lock(m_mutex);
		m_cvNotEmpty.wait(lock, [this]{ return !m_queue.empty(); });
		chunk = m_queue.front();
		m_queue.pop_front();
		m_cvNotFull.notify_one();
	}

	if (chunk.m_eof || chunk.m_error)
	{
		m_consumedCompressed	+= m_currentCompressed;		// account the final real chunk
		m_currentCompressed		 = 0;
		m_eofReached			 = true;
		return false;
	}

	// the previously held chunk is now fully consumed
	m_consumedCompressed += m_currentCompressed;
	if (m_data)
		delete[] m_data;

	m_data				= chunk.m_data;
	m_dataAvailable		= chunk.m_size;
	m_dataPos			= 0;
	m_currentCompressed	= chunk.m_compressedSize;
	return true;
}

bool BinLoader::eof()
{
	if (!m_compressed)
		return (feof(m_file) != 0);

	if (m_dataPos < m_dataAvailable)
		return false;

	// The current chunk is fully consumed. Because chunks are record-aligned (v1.3+) the
	// stream can sit exactly on a chunk boundary between records, so pull in the next chunk
	// to decide whether we are truly at the end rather than just at a boundary.
	if (m_eofReached)
		return true;

	m_bytesRead += m_dataAvailable;
	if (!loadChunk())
		return true;

	return (m_dataPos == m_dataAvailable);
}

uint64_t BinLoader::tell()
{
	if (m_compressed)
		return m_bytesRead + m_dataPos;
	else
		return fileTell();
}

uint64_t BinLoader::fileTell()
{
	if (m_compressed)
		return m_consumedCompressed;	// consumer-relative compressed position (producer reads ahead)

#if RTM_PLATFORM_WINDOWS
	uint64_t pos = (uint64_t)_ftelli64(m_file);
#elif RTM_PLATFORM_LINUX
	uint64_t pos = (uint64_t)ftello64(m_file);
#elif RTM_PLATFORM_OSX
	uint64_t pos = (uint64_t)ftello(m_file);
#endif
	return pos;
}

int BinLoader::read(void* _ptr, size_t _size)
{
	if (!m_compressed)
		return (int)fread(_ptr, _size, 1, m_file);

	const int32_t bytesLeft = m_dataAvailable - m_dataPos;

	if (bytesLeft > (int32_t)_size)
	{
		memcpy(_ptr, &m_data[m_dataPos], _size);
		m_dataPos	+= (int32_t)_size;
		return 1;
	}
	else
	{
		uint8_t* dst = (uint8_t*)_ptr;
		if (bytesLeft)
		{
			memcpy(dst, &m_data[m_dataPos], bytesLeft);
			m_dataPos	+= bytesLeft;
		}

		dst += bytesLeft;

		int32_t remaining = (int32_t)_size - bytesLeft;

		// A single request may span more than one chunk (only for pre-v1.3 captures, where
		// records are not chunk-aligned), and a chunk may hold fewer than 'remaining' bytes,
		// so walk chunks and never copy more than the current chunk actually contains.
		while (true)
		{
			if (m_dataPos == m_dataAvailable)
			{
				m_bytesRead += m_dataAvailable;
				if (!loadChunk())
					return (remaining == 0) ? 1 : 0;
				m_dataPos = 0;
			}

			if (remaining <= 0)
				break;

			const int32_t avail  = m_dataAvailable - m_dataPos;
			const int32_t toCopy = (remaining < avail) ? remaining : avail;
			if (toCopy <= 0)
				return 0; // empty/short chunk - cannot make progress

			memcpy(dst, &m_data[m_dataPos], toCopy);
			m_dataPos	+= toCopy;
			dst			+= toCopy;
			remaining	-= toCopy;
		}
		return 1;
	}
}

} // namespace rtm
