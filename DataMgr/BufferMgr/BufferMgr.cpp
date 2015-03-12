/**
 * @file    BufferMgr.cpp
 * @author  Steven Stewart <steve@map-d.com>
 * @author  Todd Mostak <todd@map-d.com>
 */
#include "BufferMgr.h"
#include "Buffer.h"

#include <algorithm>
#include <cassert>
#include <limits>

using namespace std;


namespace Buffer_Namespace {

    /// Allocates memSize bytes for the buffer pool and initializes the free memory map.
    BufferMgr::BufferMgr(const int deviceId, const size_t maxBufferSize, const size_t slabSize, const size_t pageSize, AbstractBufferMgr *parentMgr): AbstractBufferMgr(deviceId),  maxBufferSize_(maxBufferSize), slabSize_(slabSize), pageSize_(pageSize), parentMgr_(parentMgr), maxBufferId_(0), bufferEpoch_(0) {
        assert(maxBufferSize_ > 0 && slabSize_ > 0 && pageSize_ > 0 && slabSize_ % pageSize_ == 0);
        numPagesPerSlab_ = slabSize_ / pageSize_; 
        maxNumSlabs_ = (maxBufferSize_/slabSize_);
        //addSlab(slabSize_);
    }

    /// Frees the heap-allocated buffer pool memory
    BufferMgr::~BufferMgr() {
        clear();
    }

    void BufferMgr::clear() {
        std::lock_guard < std::mutex > chunkIndexLock (chunkIndexMutex_);
        std::lock_guard < std::mutex > unsizedSegsLock (unsizedSegsMutex_);
        std::lock_guard < std::mutex > sizedSegsLock (sizedSegsMutex_);
        for (auto bufferIt = chunkIndex_.begin(); bufferIt != chunkIndex_.end(); ++bufferIt) {
            delete bufferIt->second->buffer;
        }
        chunkIndex_.clear();
        //size_t numBufferSlabs = slabSegments_.size();
        slabs_.clear();
        slabSegments_.clear();
        //addSlab(slabSize_);
        unsizedSegs_.clear();
        bufferEpoch_ = 0;
    }
    
    /// Throws a runtime_error if the Chunk already exists
    AbstractBuffer * BufferMgr::createBuffer(const ChunkKey &chunkKey, const size_t chunkPageSize, const size_t initialSize) {
        size_t actualChunkPageSize = chunkPageSize;
        if (actualChunkPageSize == 0) {
            actualChunkPageSize = pageSize_;
        }

        // ChunkPageSize here is just for recording dirty pages
        {
            std::lock_guard < std::mutex > lock (chunkIndexMutex_);
            if (chunkIndex_.find(chunkKey) != chunkIndex_.end()) {
                throw std::runtime_error("Chunk already exists");
            }
            BufferSeg bufferSeg(BufferSeg(-1,0,USED));
            bufferSeg.chunkKey = chunkKey;
            std::lock_guard < std::mutex > unsizedSegsLock (unsizedSegsMutex_);
            unsizedSegs_.push_back(bufferSeg); // race condition?
            chunkIndex_[chunkKey] = std::prev(unsizedSegs_.end(),1); // need to do this before allocating Buffer because doing so could change the segment used
        }
        // following should be safe outside the lock b/c first thing Buffer
        // constructor does is pin (and its still in unsized segs at this point
        // so can't be evicted)
        allocateBuffer(chunkIndex_[chunkKey],actualChunkPageSize,initialSize); 
        //chunkIndex_[chunkKey]->buffer->pin();
        std::lock_guard < std::mutex > lock (chunkIndexMutex_);
        return chunkIndex_[chunkKey]->buffer;
    }

    BufferList::iterator BufferMgr::evict(BufferList::iterator &evictStart, const size_t numPagesRequested, const int slabNum) {
        // We can assume here that buffer for evictStart either doesn't exist
        // (evictStart is first buffer) or was not free, so don't need ot merge
        // it
        auto evictIt = evictStart;
        size_t numPages = 0;
        size_t startPage = evictStart->startPage;
        while (numPages < numPagesRequested) {
            if (evictIt->memStatus == USED) {
                assert (evictIt->buffer->getPinCount() < 1);
            }
            numPages += evictIt->numPages;
            if (evictIt->memStatus == USED && evictIt->chunkKey.size() > 0) {
                chunkIndex_.erase(evictIt->chunkKey);
            }
            evictIt = slabSegments_[slabNum].erase(evictIt); // erase operations returns next iterator - safe if we ever move to a vector (as opposed to erase(evictIt++)
        }
        BufferSeg dataSeg(startPage,numPagesRequested,USED,bufferEpoch_++); // until we can 
        //dataSeg.pinCount++;
        dataSeg.slabNum = slabNum;
        auto dataSegIt = slabSegments_[slabNum].insert(evictIt,dataSeg); // Will insert before evictIt
        if (numPagesRequested < numPages) {
            size_t excessPages = numPages - numPagesRequested;
            if (evictIt != slabSegments_[slabNum].end() && evictIt->memStatus == FREE) { // need to merge with current page
                evictIt->startPage = startPage + numPagesRequested;
                evictIt->numPages += excessPages;
            }
            else { // need to insert a free seg before evictIt for excessPages
                BufferSeg freeSeg(startPage + numPagesRequested,excessPages,FREE);
                slabSegments_[slabNum].insert(evictIt,freeSeg);
            }
        }
        return dataSegIt;
    }

    BufferList::iterator BufferMgr::reserveBuffer(BufferList::iterator &segIt, const size_t numBytes) { // assumes buffer is already pinned
        std::unique_lock < std::mutex > sizedSegsLock (sizedSegsMutex_); // inefficient but should be ok for now - particularly if different gpus are feeding from different cpu buffer pools

        // doesn't resize to be smaller - like std::reserve
        size_t numPagesRequested = (numBytes + pageSize_ - 1) / pageSize_;
        //cout << "Reserve actual number bytes: " << numPagesRequested * pageSize_ << endl;
        size_t numPagesExtraNeeded = numPagesRequested -  segIt->numPages;
       
        //cout << "Num extra pages needed: " << numPagesExtraNeeded << endl;
        if (numPagesRequested < segIt->numPages) { // We already have enough pages in existing segment
            return segIt;
        }
        // First check for freeSeg after segIt 
        int slabNum = segIt->slabNum;
        if (slabNum >= 0) { //not dummy page
            BufferList::iterator nextIt = std::next(segIt);
            if (nextIt != slabSegments_[slabNum].end() && nextIt->memStatus == FREE && nextIt->numPages >= numPagesExtraNeeded) { // Then we can just use the next BufferSeg which happens to be free
                size_t leftoverPages = nextIt->numPages - numPagesExtraNeeded;
                segIt->numPages = numPagesRequested;
                nextIt->numPages = leftoverPages;
                nextIt->startPage = segIt->startPage + segIt->numPages;
                return segIt;
            }
        }
        /* If we're here then we couldn't keep
         * buffer in existing slot - need to find
         * new segment, copy data over, and then
         * delete old
         */
        
        auto newSegIt = findFreeBuffer(numBytes);
        /* Below should be in copy constructor for BufferSeg?*/
        newSegIt->buffer = segIt->buffer;
        //newSegIt->buffer->segIt_ = newSegIt;
        newSegIt->chunkKey = segIt->chunkKey;
        int8_t * oldMem = newSegIt->buffer->mem_;
        newSegIt->buffer->mem_ = slabs_[newSegIt->slabNum] + newSegIt->startPage * pageSize_;

        // now need to copy over memory
        // only do this if the old segment is valid (i.e. not new w/
        // unallocated buffer
        if (segIt->startPage >= 0 && segIt->buffer->mem_ != 0)  {
            newSegIt->buffer->writeData(oldMem, newSegIt->buffer->size(),0,newSegIt->buffer->getType(),deviceId_);
            //memcpy(newSegIt->buffer->mem_, segIt->buffer->mem_, newSegIt->buffer->size());
        }
        // Deincrement pin count to reverse effect above
        removeSegment(segIt);
        /*
        if (segIt->slabNum < 0) {
            unsizedSegs_.erase(segIt);
        }
        else {
            slabSegments_[segIt->slabNum].erase(segIt);
            //@todo - need to delete chunks around it
        }
        */
        sizedSegsLock.unlock();
        {
            std::lock_guard < std::mutex > lock (chunkIndexMutex_); 
            chunkIndex_[newSegIt->chunkKey] = newSegIt; 
        }

        return newSegIt;
    }

    BufferList::iterator BufferMgr::findFreeBufferInSlab(const size_t slabNum, const size_t numPagesRequested) {
            for (auto bufferIt = slabSegments_[slabNum].begin(); bufferIt != slabSegments_[slabNum].end(); ++bufferIt) {
                if (bufferIt->memStatus == FREE && bufferIt->numPages >= numPagesRequested) {
                    // startPage doesn't change
                    size_t excessPages = bufferIt->numPages - numPagesRequested;
                    bufferIt->numPages = numPagesRequested;
                    bufferIt->memStatus = USED;
                    bufferIt->lastTouched  = bufferEpoch_++;
                    bufferIt->slabNum = slabNum;
                    if (excessPages > 0) {
                        BufferSeg freeSeg(bufferIt->startPage+numPagesRequested,excessPages,FREE);
                        auto tempIt = bufferIt; // this should make a copy and not be a reference
                        // - as we do not want to increment bufferIt
                        tempIt++;
                        slabSegments_[slabNum].insert(tempIt,freeSeg);
                    }
                    //std::cout << "Find free bufferIt: " << std::endl;
                    //printSeg(bufferIt);
                    return bufferIt;
                }
            }
            // If here then we did not find a free buffer of
            // sufficient size in
            // this slab, return the end iterator
            return slabSegments_[slabNum].end();
    }

    BufferList::iterator BufferMgr::findFreeBuffer(size_t numBytes) {
        size_t numPagesRequested = (numBytes + pageSize_ - 1) / pageSize_;
        if (numPagesRequested > numPagesPerSlab_) {
            throw std::runtime_error("Requtested memory allocation larger than slab size.");
        }
        
        size_t numSlabs = slabSegments_.size();

        for (size_t slabNum = 0; slabNum != numSlabs; ++slabNum) {
            auto segIt = findFreeBufferInSlab(slabNum,numPagesRequested);
            if (segIt != slabSegments_[slabNum].end()) {
                return segIt;
            }
        }

        // If we're here then we didn't find a free segment of sufficient size
        // First we see if we can add another slab
        if (numSlabs < maxNumSlabs_) {
            addSlab(slabSize_);
            return findFreeBufferInSlab(numSlabs, numPagesRequested); // has to return a free slab as long as requested buffer is smaller than the size of a slab
        }
        
        // If here then we can't add a slab - so we need to evict
        
        unsigned int minScore = std::numeric_limits<unsigned int>::max(); 
        // We're going for lowest score here, like golf
        // This is because score is the sum of the lastTouched score for all
        // pages evicted. Evicting less pages and older pages will lower the
        // score
        BufferList::iterator bestEvictionStart = slabSegments_[0].end();
        int bestEvictionStartSlab = -1;
        int slabNum = 0;

        for (auto slabIt = slabSegments_.begin(); slabIt != slabSegments_.end(); ++slabIt, ++slabNum) {
            for (auto bufferIt = slabIt->begin(); bufferIt != slabIt->end(); ++bufferIt) {
                /* Note there are some shortcuts we could take here - like we
                 * should never consider a USED buffer coming after a free buffer
                 * as we would have used the FREE buffer, but we won't worry about
                 * this for now
                 */

                // We can't evict pinned  buffers - only normal used
                // buffers

                //if (bufferIt->memStatus == FREE || bufferIt->buffer->getPinCount() == 0) {
                    size_t pageCount = 0;
                    size_t score = 0;
                    bool solutionFound = false;
                    auto evictIt = bufferIt;
                    for (; evictIt != slabSegments_[slabNum].end(); ++evictIt) {
                        // pinCount should never go up - only down because we have
                        // global lock on buffer pool and pin count only increments
                        // on getChunk
                        if (evictIt->memStatus == USED && evictIt->buffer->getPinCount() > 0) {
                           break;
                        }
                        pageCount += evictIt->numPages;
                        if (evictIt->memStatus == USED) {
                            score += evictIt->lastTouched;
                        }
                        if (pageCount >= numPagesRequested) {
                            solutionFound = true;
                            break;
                        }
                    }
                    if (solutionFound && score < minScore) {
                        minScore = score;
                        bestEvictionStart = bufferIt;
                        bestEvictionStartSlab = slabNum;
                    }
                    else if (evictIt == slabSegments_[slabNum].end()) {
                        // this means that every segment after this will fail as
                        // well, so our search has proven futile
                        //throw std::runtime_error ("Couldn't evict chunks to get free space");
                        break;
                        // in reality we should try to rearrange the buffer to get
                        // more contiguous free space
                    }
                    // other possibility is ending at PINNED - do nothing in this
                    // case

                //}
            }
        }
        if (bestEvictionStart == slabSegments_[0].end()) {
            throw std::runtime_error ("Couldn't evict chunks to get free space");
        }
        bestEvictionStart = evict(bestEvictionStart,numPagesRequested,bestEvictionStartSlab);
        return bestEvictionStart;
    }

    void BufferMgr::printSeg(BufferList::iterator &segIt) {

        std::cout << "Start page: " << segIt->startPage << std::endl;
        std::cout << "Num Pages: " << segIt->numPages << std::endl;
        std::cout << "Last touched: " << segIt->lastTouched << std::endl;
        if (segIt->memStatus == FREE)
            std::cout << "FREE" << std::endl;
        else {
            std::cout << "USED - Chunk: ";
            for (auto vecIt = segIt->chunkKey.begin(); vecIt != segIt->chunkKey.end(); ++vecIt) {
                std::cout << *vecIt << ",";
            }
            std::cout << std::endl;
            std::cout << "Pin count: " << segIt->buffer->getPinCount() << std::endl;
            //printf ("Mem address:  %p",segIt->buffer->getMemoryPtr());
            std::cout << "t: " << segIt->buffer->getPinCount() << std::endl;

        }
    }




    void BufferMgr::printSegs() {
        int segNum = 1;
        int slabNum = 1;
        std::cout << std::endl << std::endl;
        for (auto slabIt = slabSegments_.begin(); slabIt != slabSegments_.end(); ++slabIt,++slabNum) {
            std::cout << "Slab Num: " << slabNum << std::endl;
            for (auto segIt = slabIt->begin(); segIt != slabIt->end(); ++segIt,++segNum) {
                std::cout << "Segment: " << segNum << std::endl;
                printSeg(segIt);
                std::cout << std::endl;
            }
            std::cout << "--------------------" << std::endl;
        }
    }

    void BufferMgr::printMap() {
        int segNum = 1;
        std::cout << std::endl << "Map Contents: " <<  std::endl;
        for (auto segIt = chunkIndex_.begin(); segIt != chunkIndex_.end(); ++segIt,++segNum) {
            std::cout << "Chunk " << segNum << ": ";
            for (auto vecIt = segIt->first.begin(); vecIt != segIt->first.end(); ++vecIt) {
                std::cout << *vecIt << ",";
            }
            std::cout << std::endl;
            printSeg(segIt->second);
        }
        std::cout << "--------------------" << std::endl;
    }

    bool BufferMgr::isBufferOnDevice(const ChunkKey &key) {
        std::lock_guard < std::mutex > chunkIndexLock (chunkIndexMutex_);
        if (chunkIndex_.find(key) == chunkIndex_.end()) {
            return false;
        }
        else {
            return true;
        }
    }

    /// This method throws a runtime_error when deleting a Chunk that does not exist.
    void BufferMgr::deleteBuffer(const ChunkKey &key, const bool purge) { 
        std::unique_lock < std::mutex > chunkIndexLock (chunkIndexMutex_);
        // Note: purge is currently unused

        // lookup the buffer for the Chunk in chunkIndex_
        auto bufferIt = chunkIndex_.find(key);
        //Buffer *buffer = bufferIt->second->buffer;

        if (chunkIndex_.find(key) == chunkIndex_.end()) {
            throw std::runtime_error("Chunk does not exist");
        }
        auto  segIt = bufferIt->second;
        chunkIndex_.erase(bufferIt);
        chunkIndexLock.unlock();
        std::lock_guard < std::mutex > sizedSegsLock (sizedSegsMutex_);
        delete segIt->buffer; // Delete Buffer for segment
        segIt->buffer = 0;
        removeSegment(segIt);
    }

    void BufferMgr::deleteBuffersWithPrefix(const ChunkKey &keyPrefix, const bool purge) {
        // Note: purge is unused
        // lookup the buffer for the Chunk in chunkIndex_
        std::lock_guard < std::mutex > sizedSegsLock (sizedSegsMutex_); // Take this lock early to prevent deadlock with reserveBuffer which needs segsMutex_ and then chunkIndexMutex_
        std::lock_guard < std::mutex > chunkIndexLock (chunkIndexMutex_);
        auto startChunkIt = chunkIndex_.lower_bound(keyPrefix);
        if (startChunkIt == chunkIndex_.end()) {
            return;
        }

        auto bufferIt = startChunkIt;
        while (bufferIt != chunkIndex_.end() && std::search(bufferIt->first.begin(),bufferIt->first.begin()+keyPrefix.size(),keyPrefix.begin(),keyPrefix.end()) != bufferIt->first.begin()+keyPrefix.size()) {
        //cout << "Before getting segIt" << endl;
        auto  segIt = bufferIt->second;
        delete segIt->buffer; // Delete Buffer for segment
        segIt->buffer = 0;
        removeSegment(segIt);
        chunkIndex_.erase(bufferIt++);
        }
    }

     void BufferMgr::removeSegment(BufferList::iterator &segIt) {
         // Note: does not delete buffer as this may be moved somewhere else
        int slabNum = segIt->slabNum;
        //cout << "Slab num: " << slabNum << endl;
        if (slabNum < 0) {
            std::lock_guard < std::mutex > unsizedSegsLock (unsizedSegsMutex_);
            unsizedSegs_.erase(segIt);
        }
        else {
            if (segIt != slabSegments_[slabNum].begin()) {
                auto prevIt = std::prev(segIt);
                //std::cout << "PrevIt: " << std::endl;
                //printSeg(prevIt);
                if (prevIt->memStatus == FREE) { 
                    segIt->startPage = prevIt->startPage;
                    segIt->numPages += prevIt->numPages; 
                    slabSegments_[slabNum].erase(prevIt);
                }
            }
            auto nextIt = std::next(segIt);
            if (nextIt != slabSegments_[slabNum].end()) {
                if (nextIt->memStatus == FREE) { 
                    segIt->numPages += nextIt->numPages;
                    slabSegments_[slabNum].erase(nextIt);
                }
            }
            segIt->memStatus = FREE;
            //segIt->pinCount = 0;
            segIt->buffer = 0;
        }

    }

    void BufferMgr::checkpoint() {
        std::lock_guard < std::mutex > sizedSegsLock (sizedSegsMutex_); // Take this lock early to prevent deadlock with reserveBuffer which needs segsMutex_ and then chunkIndexMutex_
        std::lock_guard < std::mutex > chunkIndexLock (chunkIndexMutex_);
        for (auto bufferIt = chunkIndex_.begin(); bufferIt != chunkIndex_.end(); ++bufferIt) {
            if (bufferIt->second->chunkKey[0] != -1 && bufferIt->second->buffer->isDirty_) { // checks that buffer is actual chunk (not just buffer) and is dirty
                //cout << "Flushing: ";
                //for (auto vecIt = bufferIt->second->chunkKey.begin(); vecIt != bufferIt->second->chunkKey.end(); ++vecIt) {
                //    std::cout << *vecIt << ",";
                //}
                //std::cout << std::endl;
                parentMgr_->putBuffer(bufferIt->second->chunkKey, bufferIt->second->buffer); 
                bufferIt->second->buffer->clearDirtyBits();
            }
        }
    }
    
    /// Returns a pointer to the Buffer holding the chunk, if it exists; otherwise,
    /// throws a runtime_error.
    AbstractBuffer* BufferMgr::getBuffer(const ChunkKey &key, const size_t numBytes) {
        std::unique_lock < std::mutex > sizedSegsLock (sizedSegsMutex_); 
        std::unique_lock < std::mutex > chunkIndexLock (chunkIndexMutex_);
        
        auto bufferIt = chunkIndex_.find(key);
        bool foundBuffer = bufferIt != chunkIndex_.end();
        chunkIndexLock.unlock();
        if (foundBuffer) {
            bufferIt->second->buffer->pin();
            sizedSegsLock.unlock();
            bufferIt->second->lastTouched = bufferEpoch_++; // race
            if (bufferIt->second->buffer->size() < numBytes) {  // need to fetch part of buffer we don't have - up to numBytes 
                parentMgr_->fetchBuffer(key, bufferIt->second->buffer, numBytes);
            }
            return bufferIt->second->buffer; 
        }
        else { // If wasn't in pool then we need to fetch it
            sizedSegsLock.unlock();
            AbstractBuffer * buffer = createBuffer(key,pageSize_,numBytes); // createChunk pins for us
            try {
                parentMgr_->fetchBuffer(key,buffer,numBytes); // this should put buffer in a BufferSegment
            }
            catch (std::runtime_error &error) {
                // if here, fetch chunk was unsuccessful - delete chunk we just
                // created
                deleteBuffer(key);
                printMap();
                cout << endl << endl;
                printSegs();
                cout << error.what() << endl;


                throw std::runtime_error("Get chunk - Could not find chunk in buffer pool or parent buffer pools");
            }
            return buffer;
        }
    }


    void BufferMgr::fetchBuffer(const ChunkKey &key, AbstractBuffer *destBuffer, const size_t numBytes) {
        std::unique_lock < std::mutex > sizedSegsLock (sizedSegsMutex_); 
        std::unique_lock < std::mutex > chunkIndexLock (chunkIndexMutex_);
        
        auto bufferIt = chunkIndex_.find(key);
        bool foundBuffer = bufferIt != chunkIndex_.end();
        chunkIndexLock.unlock();
        AbstractBuffer * buffer;
        if (!foundBuffer) {
            sizedSegsLock.unlock();
            if (parentMgr_ == 0) {
                throw std::runtime_error("Chunk does not exist");
            }
            buffer = createBuffer(key,pageSize_,numBytes); // will pin buffer
            try {
                parentMgr_->fetchBuffer(key, buffer, numBytes);
            }
            catch (std::runtime_error &error) {
                deleteBuffer(key);
                throw std::runtime_error("Fetch chunk - Could not find chunk in buffer pool or parent buffer pools");
            }
        }
        else {
            buffer = bufferIt->second->buffer;
            buffer->pin();
            sizedSegsLock.unlock();
        }
        size_t chunkSize = numBytes == 0 ? buffer->size() : numBytes;
        destBuffer->reserve(chunkSize);
        //std::cout << "After reserve chunksize: " << chunkSize << std::endl;
        if (buffer->isUpdated()) {
            buffer->read(destBuffer->getMemoryPtr(),chunkSize,0,destBuffer->getType(),deviceId_);
        }
        else {
            buffer->read(destBuffer->getMemoryPtr()+destBuffer->size(),chunkSize-destBuffer->size(), destBuffer->size(), destBuffer->getType(), deviceId_);
        }
        destBuffer->setSize(chunkSize);
        destBuffer->syncEncoder(buffer);
        buffer->unPin();
    }

    AbstractBuffer* BufferMgr::putBuffer(const ChunkKey &key, AbstractBuffer *srcBuffer, const size_t numBytes) {
        std::unique_lock < std::mutex > chunkIndexLock (chunkIndexMutex_);
        auto bufferIt = chunkIndex_.find(key);
        bool foundBuffer = bufferIt != chunkIndex_.end();
        chunkIndexLock.unlock();
        AbstractBuffer *buffer;
        if (!foundBuffer) {
            buffer = createBuffer(key,pageSize_);
        }
        else {
            buffer = bufferIt->second->buffer;
        }
        size_t oldBufferSize = buffer->size();
        size_t newBufferSize = numBytes == 0 ? srcBuffer->size() : numBytes;

        if (buffer->isDirty()) {
            throw std::runtime_error("Chunk inconsistency");
        }

        if (srcBuffer->isUpdated()) {
            //@todo use dirty flags to only flush pages of chunk that need to
            //be flushed
            buffer->write((int8_t *)srcBuffer->getMemoryPtr(), newBufferSize,0,srcBuffer->getType(),srcBuffer->getDeviceId());
        }
        else if (srcBuffer->isAppended()) {
            assert(oldBufferSize < newBufferSize);
            buffer->append((int8_t *)srcBuffer->getMemoryPtr()+oldBufferSize,newBufferSize-oldBufferSize,srcBuffer->getType(), srcBuffer->getDeviceId());
        }
        srcBuffer->clearDirtyBits();
        buffer->syncEncoder(srcBuffer);
        return buffer;
    }

    int BufferMgr::getBufferId() {
        std::lock_guard < std::mutex > lock (bufferIdMutex_);
        return maxBufferId_++;
    }
    
    /// client is responsible for deleting memory allocated for b->mem_
    AbstractBuffer* BufferMgr::alloc(const size_t numBytes) {
        ChunkKey chunkKey = {-1,getBufferId()};
        return createBuffer(chunkKey, pageSize_, numBytes); 
    }

    void BufferMgr::free(AbstractBuffer *buffer) {
        Buffer * castedBuffer = dynamic_cast <Buffer *> (buffer); 
        if (castedBuffer == 0) {
            throw std::runtime_error ("Wrong buffer type - expects base class pointer to Buffer type");
        }
        deleteBuffer(castedBuffer->segIt_->chunkKey);
    }
    
    size_t BufferMgr::getNumChunks() {
        std::lock_guard < std::mutex > chunkIndexLock (chunkIndexMutex_);
        return chunkIndex_.size();
    }
    
    size_t BufferMgr::size() {
        std::lock_guard < std::mutex > sizedSegsLock (sizedSegsMutex_);
        return slabs_.size()*pageSize_*numPagesPerSlab_;
    }
    void BufferMgr::getChunkMetadataVec(std::vector<std::pair<ChunkKey,ChunkMetadata> > &chunkMetadataVec) {
        throw std::runtime_error ("getChunkMetadataVec not supported for BufferMgr");
    }

    void BufferMgr::getChunkMetadataVecForKeyPrefix(std::vector<std::pair<ChunkKey,ChunkMetadata> > &chunkMetadataVec, const ChunkKey &keyPrefix) {
        throw std::runtime_error ("getChunkMetadataVecForPrefix not supported for BufferMgr");
    }
}
