//     ____   ______ __
//    / __ \ / ____// /
//   / /_/ // /    / /
//  / ____// /___ / /___   PixInsight Class Library
// /_/     \____//_____/   PCL 02.01.01.0784
// ----------------------------------------------------------------------------
// pcl/Compression.cpp - Released 2016/02/21 20:22:19 UTC
// ----------------------------------------------------------------------------
// This file is part of the PixInsight Class Library (PCL).
// PCL is a multiplatform C++ framework for development of PixInsight modules.
//
// Copyright (c) 2003-2016 Pleiades Astrophoto S.L. All Rights Reserved.
//
// Redistribution and use in both source and binary forms, with or without
// modification, is permitted provided that the following conditions are met:
//
// 1. All redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
//
// 2. All redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//
// 3. Neither the names "PixInsight" and "Pleiades Astrophoto", nor the names
//    of their contributors, may be used to endorse or promote products derived
//    from this software without specific prior written permission. For written
//    permission, please contact info@pixinsight.com.
//
// 4. All products derived from this software, in any form whatsoever, must
//    reproduce the following acknowledgment in the end-user documentation
//    and/or other materials provided with the product:
//
//    "This product is based on software from the PixInsight project, developed
//    by Pleiades Astrophoto and its contributors (http://pixinsight.com/)."
//
//    Alternatively, if that is where third-party acknowledgments normally
//    appear, this acknowledgment must be reproduced in the product itself.
//
// THIS SOFTWARE IS PROVIDED BY PLEIADES ASTROPHOTO AND ITS CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
// TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL PLEIADES ASTROPHOTO OR ITS
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, BUSINESS
// INTERRUPTION; PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; AND LOSS OF USE,
// DATA OR PROFITS) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
// ----------------------------------------------------------------------------

#include <pcl/AutoLock.h>
#include <pcl/Compression.h>
#include <pcl/ElapsedTime.h>
#include <pcl/Exception.h>
#include <pcl/ReferenceArray.h>
#include <pcl/StringList.h>
#include <pcl/Thread.h>

#include <pcl/api/APIInterface.h>

namespace pcl
{

// ----------------------------------------------------------------------------

class PCL_CompressionEngine
{
public:

   PCL_CompressionEngine( const Compression& compression ) :
      m_compression( compression )
   {
   }

   Compression::subblock_list operator ()( const void* data, size_type size, Compression::Performance* perf )
   {
      if ( data == nullptr || size == 0 )
         return Compression::subblock_list();

      m_data = reinterpret_cast<ByteArray::const_iterator>( data );

      m_compressionLevel = m_compression.CompressionLevel();
      if ( m_compressionLevel <= 0 )
         m_compressionLevel = m_compression.DefaultCompressionLevel();
      else
         m_compressionLevel = Range( m_compressionLevel, 1, m_compression.MaxCompressionLevel() );

      m_subblockSize = m_compression.SubblockSize();
      if ( m_subblockSize < m_compression.MinBlockSize() || m_subblockSize > m_compression.MaxBlockSize() )
         m_subblockSize = m_compression.MaxBlockSize();

      m_numberOfSubblocks = size / m_subblockSize;
      m_remainingSize = size % m_subblockSize;

      ElapsedTime T;
      double dt = 0;

      ByteArray S;
      if ( m_compression.ByteShufflingEnabled() && m_compression.ItemSize() > 1 )
      {
         T.Reset();
         S = Compression::Shuffle( m_data, size, m_compression.ItemSize() );
         dt += T();
         m_data = S.Begin();
      }

      int numberOfThreads = m_compression.IsParallelProcessingEnabled() ?
               Min( m_compression.MaxProcessors(), pcl::Thread::NumberOfThreads( m_numberOfSubblocks + 1, 1 ) ) : 1;
      int subblocksPerThread = int( m_numberOfSubblocks + 1 )/numberOfThreads;

      ReferenceArray<CompressionThread> threads;
      for ( int i = 0, j = 1; i < numberOfThreads; ++i, ++j )
         threads.Add( new CompressionThread( *this,
                                             i*subblocksPerThread,
                                             (j < numberOfThreads) ? j*subblocksPerThread : m_numberOfSubblocks + 1 ) );
      m_errors.Clear();

      T.Reset();

      if ( numberOfThreads > 1 )
      {
         for ( int i = 0; i < numberOfThreads; ++i )
            threads[i].Start( ThreadPriority::DefaultMax, i );
         for ( int i = 0; i < numberOfThreads; ++i )
            threads[i].Wait();
      }
      else
         threads[0].Run();

      if ( !m_errors.IsEmpty() )
      {
         threads.Destroy();
         m_compression.Throw( String().ToSeparated( m_errors, '\n' ) );
      }

      dt += T();

      Compression::subblock_list subblocks;
      for ( int i = 0; i < numberOfThreads; ++i )
         subblocks.Append( threads[i].subblocks );

      threads.Destroy();

      size_type compressedSize = 0;
      for ( Compression::subblock_list::const_iterator i = subblocks.Begin(); i != subblocks.End(); ++i )
         compressedSize += i->compressedData.Length();
      compressedSize += subblocks.Size() + sizeof( Compression::subblock_list );

      if ( perf != nullptr )
      {
         perf->sizeReduction = double( size - compressedSize )/size;
         perf->throughput = size/dt/1024/1024; // MiB/s
         perf->numberOfThreads = numberOfThreads;
      }

      if ( compressedSize >= size )
         subblocks.Clear();

      return subblocks;
   }

private:

     const Compression&              m_compression;
           int                       m_compressionLevel;
           size_type                 m_numberOfSubblocks;
           size_type                 m_subblockSize;
           size_type                 m_remainingSize;
           ByteArray::const_iterator m_data;
   mutable Mutex                     m_mutex;
   mutable StringList                m_errors;

   class CompressionThread : public Thread
   {
   public:

      Compression::subblock_list subblocks;

      CompressionThread( const PCL_CompressionEngine& engine, size_type beginSubblock, size_type endSubblock ) :
         E( engine ),
         m_beginSubblock( beginSubblock ),
         m_endSubblock( endSubblock )
      {
      }

      virtual void Run()
      {
         try
         {
            for ( size_type i = m_beginSubblock; i < m_endSubblock; ++i )
            {
               Compression::Subblock subblock;
               subblock.uncompressedSize = (i < E.m_numberOfSubblocks) ? E.m_subblockSize : E.m_remainingSize;
               if ( subblock.uncompressedSize > 0 )
               {
                  ByteArray::const_iterator uncompressedBegin = E.m_data + i*E.m_subblockSize;
                  if ( subblock.uncompressedSize >= E.m_compression.MinBlockSize() )
                  {
                     ByteArray compressedData( E.m_compression.MaxCompressedBlockSize( subblock.uncompressedSize ) );
                     size_type compressedSize = E.m_compression.CompressBlock( compressedData.Begin(), compressedData.Length(),
                                                      uncompressedBegin, subblock.uncompressedSize, E.m_compressionLevel );
                     if ( compressedSize > 0 && compressedSize < subblock.uncompressedSize )
                     {
                        // Compressed subblock.
                        subblock.compressedData = ByteArray( compressedData.Begin(), compressedData.At( compressedSize ) );
                        goto __nextBlock;
                     }
                  }

                  // Sub-block too small to be compressed, or data not compressible.
                  subblock.compressedData = ByteArray( uncompressedBegin, uncompressedBegin + subblock.uncompressedSize );

         __nextBlock:

                  if ( E.m_compression.ChecksumsEnabled() )
                     subblock.checksum = subblock.compressedData.Hash64();

                  subblocks.Append( subblock );
               }
            }
         }
         catch ( ... )
         {
            volatile AutoLock lock( E.m_mutex );

            try
            {
               throw;
            }
            catch ( Exception& x )
            {
               E.m_errors << x.Message();
            }
            catch ( std::bad_alloc& )
            {
               E.m_errors << "Out of memory";
            }
            catch ( ... )
            {
               E.m_errors << "Unknown error";
            }
         }
      }

   private:

      const PCL_CompressionEngine& E;
            size_type              m_beginSubblock;
            size_type              m_endSubblock;
   };
};

Compression::subblock_list Compression::Compress( const void* data, size_type size, Compression::Performance* perf ) const
{
   return PCL_CompressionEngine( *this )( data, size, perf );
}

// ----------------------------------------------------------------------------

class PCL_DecompressionEngine
{
public:

   PCL_DecompressionEngine( const Compression& compression ) :
      m_compression( compression )
   {
   }

   size_type operator ()( void* data, size_type maxSize,
                          const Compression::subblock_list& subblocks, Compression::Performance* perf )
   {
      if ( subblocks.IsEmpty() )
         return 0;

      size_type uncompressedSize = 0;
      for ( Compression::subblock_list::const_iterator i = subblocks.Begin(); i != subblocks.End(); ++i )
      {
         if ( i->compressedData.IsEmpty() || i->uncompressedSize == 0 )
            m_compression.Throw( "Invalid compressed subblock data." );
         uncompressedSize += i->uncompressedSize;
      }
      if ( maxSize < uncompressedSize )
         m_compression.Throw( String().Format( "Insufficient uncompression buffer length (required %llu, available %llu)",
                                               uncompressedSize, maxSize ) );

      m_uncompressedData = reinterpret_cast<ByteArray::iterator>( data );

      int numberOfThreads = m_compression.IsParallelProcessingEnabled() ?
               Min( m_compression.MaxProcessors(), pcl::Thread::NumberOfThreads( subblocks.Length(), 1 ) ) : 1;
      int subblocksPerThread = int( subblocks.Length() )/numberOfThreads;

      ReferenceArray<DecompressionThread> threads;
      size_type offset = 0;
      for ( int i = 0, j = 1; i < numberOfThreads; ++i, ++j )
      {
         Compression::subblock_list::const_iterator begin = subblocks.At( i*subblocksPerThread );
         Compression::subblock_list::const_iterator end = (j < numberOfThreads) ? subblocks.At( j*subblocksPerThread ) : subblocks.End();

         threads.Add( new DecompressionThread( *this, offset, begin, end ) );

         if ( j < numberOfThreads )
            for ( Compression::subblock_list::const_iterator i = begin; i != end; ++i )
               offset += i->uncompressedSize;
      }

      m_errors.Clear();

      ElapsedTime T;

      if ( numberOfThreads > 1 )
      {
         for ( int i = 0; i < numberOfThreads; ++i )
            threads[i].Start( ThreadPriority::DefaultMax, i );
         for ( int i = 0; i < numberOfThreads; ++i )
            threads[i].Wait();
      }
      else
         threads[0].Run();

      double dt = T();

      threads.Destroy();

      if ( !m_errors.IsEmpty() )
         m_compression.Throw( String().ToSeparated( m_errors, '\n' ) );

      if ( m_compression.ByteShufflingEnabled() && m_compression.ItemSize() > 1 )
      {
         T.Reset();
         Compression::InPlaceUnshuffle( m_uncompressedData, uncompressedSize, m_compression.ItemSize() );
         dt += T();
      }

      if ( perf != nullptr )
      {
         size_type compressedSize = 0;
         for ( Compression::subblock_list::const_iterator i = subblocks.Begin(); i != subblocks.End(); ++i )
            compressedSize += i->compressedData.Length();
         compressedSize += subblocks.Size() + sizeof( Compression::subblock_list );

         perf->sizeReduction = double( uncompressedSize - compressedSize )/uncompressedSize;
         perf->throughput = uncompressedSize/dt/1024/1024; // MiB/s
         perf->numberOfThreads = numberOfThreads;
      }

      return uncompressedSize;
   }

private:

     const Compression&        m_compression;
           ByteArray::iterator m_uncompressedData;
   mutable Mutex               m_mutex;
   mutable StringList          m_errors;

   class DecompressionThread : public Thread
   {
   public:

      DecompressionThread( const PCL_DecompressionEngine& engine,
                           size_type offset,
                           Compression::subblock_list::const_iterator begin,
                           Compression::subblock_list::const_iterator end ) :
         E( engine ),
         m_offset( offset ),
         m_begin( begin ),
         m_end( end )
      {
      }

      virtual void Run()
      {
         try
         {
            size_type uncompressedSize = 0;
            for ( Compression::subblock_list::const_iterator i = m_begin; i != m_end; ++i )
               uncompressedSize += i->uncompressedSize;

            size_type totalSize = 0;
            for ( Compression::subblock_list::const_iterator i = m_begin; i != m_end; ++i )
            {
               if ( i->checksum != 0 )
               {
                  uint64 checksum = i->compressedData.Hash64();
                  if ( i->checksum != checksum )
                     throw String().Format( "Sub-block checksum mismatch (offset=%llu, expected %llx, got %llx)",
                                            m_offset+totalSize, i->checksum, checksum );
               }

               if ( i->compressedData.Length() < i->uncompressedSize )
               {
                  // Compressed subblock.
                  size_type subblockSize = E.m_compression.UncompressBlock( E.m_uncompressedData+m_offset+totalSize,
                                                                            uncompressedSize-totalSize,
                                                                            i->compressedData.Begin(), i->compressedData.Length() );
                  if ( subblockSize == 0 )
                     throw String().Format( "Failed to uncompress subblock data (offset=%llu usize=%llu csize=%llu)",
                                            m_offset+totalSize, i->uncompressedSize, i->compressedData.Length() );
                  if ( subblockSize != i->uncompressedSize )
                     throw String().Format( "Uncompressed subblock size mismatch (offset=%llu, expected %llu, got %llu)",
                                            m_offset+totalSize, i->uncompressedSize, subblockSize );
               }
               else
               {
                  // Sub-block too small to be compressed, or data not compressible.
                  ::memcpy( E.m_uncompressedData+m_offset+totalSize, i->compressedData.Begin(), i->uncompressedSize );
               }

               totalSize += i->uncompressedSize;
            }
            if ( totalSize != uncompressedSize )
               throw String().Format( "Uncompressed data size mismatch (offset=%llu, expected %llu, got %llu)",
                                      m_offset, uncompressedSize, totalSize );
         }
         catch ( ... )
         {
            volatile AutoLock lock( E.m_mutex );

            try
            {
               throw;
            }
            catch ( String& s )
            {
               E.m_errors << s;
            }
            catch ( Exception& x )
            {
               E.m_errors << x.Message();
            }
            catch ( std::bad_alloc& )
            {
               E.m_errors << "Out of memory";
            }
            catch ( ... )
            {
               E.m_errors << "Unknown error";
            }
         }
      }

   private:

      const PCL_DecompressionEngine&                   E;
            size_type                                  m_offset;
            Compression::subblock_list::const_iterator m_begin;
            Compression::subblock_list::const_iterator m_end;
   };
};

size_type Compression::Uncompress( void* data, size_type maxSize,
                                   const Compression::subblock_list& subblocks, Compression::Performance* perf ) const
{
   return PCL_DecompressionEngine( *this )( data, maxSize, subblocks, perf );
}

// ----------------------------------------------------------------------------

void Compression::Throw( const String& errorMessage ) const
{
   throw Error( AlgorithmName() + " compression: " + errorMessage );
}

// ----------------------------------------------------------------------------

#ifdef _MSC_VER // shut up, stupid!
PCL_WARNINGS_DISABLE_SIZE_T_TO_INT_LOSS
#endif

int ZLibCompression::MaxCompressionLevel() const
{
   return (*API->Compression->ZLibMaxCompressionLevel)();
}

int ZLibCompression::DefaultCompressionLevel() const
{
   return (*API->Compression->ZLibDefaultCompressionLevel)();
}

size_type ZLibCompression::MinBlockSize() const
{
   return (*API->Compression->ZLibMinUncompressedBlockSize)();
}

size_type ZLibCompression::MaxBlockSize() const
{
   return (*API->Compression->ZLibMaxUncompressedBlockSize)();
}

size_type ZLibCompression::MaxCompressedBlockSize( size_type size ) const
{
   return (*API->Compression->ZLibMaxCompressedBlockSize)( uint32( size ) );
}

size_type ZLibCompression::CompressBlock( void* outputData, size_type maxOutputSize,
                                          const void* inputData, size_type inputSize, int level ) const
{
   uint32 outputSize( maxOutputSize );
   if ( (*API->Compression->ZLibCompressBlock)( outputData, &outputSize, inputData, uint32( inputSize ), level ) != api_false )
      return outputSize;
   return 0;
}

size_type ZLibCompression::UncompressBlock( void* outputData, size_type maxOutputSize,
                                            const void* inputData, size_type inputSize ) const
{
   uint32 outputSize( maxOutputSize );
   if ( (*API->Compression->ZLibUncompressBlock)( outputData, &outputSize, inputData, uint32( inputSize ) ) != api_false )
      return outputSize;
   return 0;
}

// ----------------------------------------------------------------------------

int LZ4Compression::MaxCompressionLevel() const
{
   return (*API->Compression->LZ4MaxCompressionLevel)();
}

int LZ4Compression::DefaultCompressionLevel() const
{
   return (*API->Compression->LZ4DefaultCompressionLevel)();
}

size_type LZ4Compression::MinBlockSize() const
{
   return (*API->Compression->LZ4MinUncompressedBlockSize)();
}

size_type LZ4Compression::MaxBlockSize() const
{
   return (*API->Compression->LZ4MaxUncompressedBlockSize)();
}

size_type LZ4Compression::MaxCompressedBlockSize( size_type size ) const
{
   return (*API->Compression->LZ4MaxCompressedBlockSize)( uint32( size ) );
}

size_type LZ4Compression::CompressBlock( void* outputData, size_type maxOutputSize,
                                           const void* inputData, size_type inputSize, int level ) const
{
   uint32 outputSize( maxOutputSize );
   if ( (*API->Compression->LZ4CompressBlock)( outputData, &outputSize, inputData, uint32( inputSize ), level ) != api_false )
      return outputSize;
   return 0;
}

size_type LZ4Compression::UncompressBlock( void* outputData, size_type maxOutputSize,
                                             const void* inputData, size_type inputSize ) const
{
   uint32 outputSize( maxOutputSize );
   if ( (*API->Compression->LZ4UncompressBlock)( outputData, &outputSize, inputData, uint32( inputSize ) ) != api_false )
      return outputSize;
   return 0;
}

// ----------------------------------------------------------------------------

int LZ4HCCompression::MaxCompressionLevel() const
{
   return (*API->Compression->LZ4HCMaxCompressionLevel)();
}

int LZ4HCCompression::DefaultCompressionLevel() const
{
   return (*API->Compression->LZ4HCDefaultCompressionLevel)();
}

size_type LZ4HCCompression::MinBlockSize() const
{
   return (*API->Compression->LZ4HCMinUncompressedBlockSize)();
}

size_type LZ4HCCompression::MaxBlockSize() const
{
   return (*API->Compression->LZ4HCMaxUncompressedBlockSize)();
}

size_type LZ4HCCompression::MaxCompressedBlockSize( size_type size ) const
{
   return (*API->Compression->LZ4HCMaxCompressedBlockSize)( uint32( size ) );
}

size_type LZ4HCCompression::CompressBlock( void* outputData, size_type maxOutputSize,
                                           const void* inputData, size_type inputSize, int level ) const
{
   uint32 outputSize( maxOutputSize );
   if ( (*API->Compression->LZ4HCCompressBlock)( outputData, &outputSize, inputData, uint32( inputSize ), level ) != api_false )
      return outputSize;
   return 0;
}

size_type LZ4HCCompression::UncompressBlock( void* outputData, size_type maxOutputSize,
                                             const void* inputData, size_type inputSize ) const
{
   uint32 outputSize( maxOutputSize );
   if ( (*API->Compression->LZ4HCUncompressBlock)( outputData, &outputSize, inputData, uint32( inputSize ) ) != api_false )
      return outputSize;
   return 0;
}

// ----------------------------------------------------------------------------

int BloscLZCompression::MaxCompressionLevel() const
{
   return (*API->Compression->BloscLZMaxCompressionLevel)();
}

int BloscLZCompression::DefaultCompressionLevel() const
{
   return (*API->Compression->BloscLZDefaultCompressionLevel)();
}

size_type BloscLZCompression::MinBlockSize() const
{
   return (*API->Compression->BloscLZMinUncompressedBlockSize)();
}

size_type BloscLZCompression::MaxBlockSize() const
{
   return (*API->Compression->BloscLZMaxUncompressedBlockSize)();
}

size_type BloscLZCompression::MaxCompressedBlockSize( size_type size ) const
{
   return (*API->Compression->BloscLZMaxCompressedBlockSize)( uint32( size ) );
}

size_type BloscLZCompression::CompressBlock( void* outputData, size_type maxOutputSize,
                                             const void* inputData, size_type inputSize, int level ) const
{
   uint32 outputSize( maxOutputSize );
   if ( (*API->Compression->BloscLZCompressBlock)( outputData, &outputSize, inputData, uint32( inputSize ), level ) != api_false )
      return outputSize;
   return 0;
}

size_type BloscLZCompression::UncompressBlock( void* outputData, size_type maxOutputSize,
                                               const void* inputData, size_type inputSize ) const
{
   uint32 outputSize( maxOutputSize );
   if ( (*API->Compression->BloscLZUncompressBlock)( outputData, &outputSize, inputData, uint32( inputSize ) ) != api_false )
      return outputSize;
   return 0;
}

// ----------------------------------------------------------------------------

} // pcl

// ----------------------------------------------------------------------------
// EOF pcl/Compression.cpp - Released 2016/02/21 20:22:19 UTC
