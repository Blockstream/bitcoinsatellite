// Copyright (c) 2016, 2017 Matt Corallo
// Unlike the rest of Bitcoin Core, this file is
// distributed under the Affero General Public License (AGPL v3)

#include <udprelay.h>

#include <chainparams.h>
#include <consensus/consensus.h> // for MAX_BLOCK_SERIALIZED_SIZE
#include <consensus/validation.h> // for CValidationState
#include <logging.h>
#include <streams.h>
#include <validation.h>
#include <version.h>

#include <condition_variable>
#include <thread>
#include <boost/optional.hpp>

#include <boost/thread.hpp>

#define to_millis_double(t) (std::chrono::duration_cast<std::chrono::duration<double, std::chrono::milliseconds::period> >(t).count())
#define DIV_CEIL(a, b) (((a) + (b) - 1) / (b))

static CService TRUSTED_PEER_DUMMY;
static std::map<std::pair<uint64_t, CService>, std::shared_ptr<PartialBlockData> > mapPartialBlocks;
static std::unordered_set<uint64_t> setBlocksRelayed;
// In cases where we receive a block without its previous block, or a block
// which is already (to us) an orphan, we will not get a UDPRelayBlock
// callback. However, we do not want to re-process the still-happening stream
// of packets into more ProcessNewBlock calls, so we have to keep a separate
// set here.
static std::set<std::pair<uint64_t, CService>> setBlocksReceived;
// Keep track of used vs. received block chunks
static std::map<std::pair<uint64_t, CService>, BlockChunkCount> mapChunkCount;

static std::map<std::pair<uint64_t, CService>, std::shared_ptr<PartialBlockData> >::iterator RemovePartialBlock(std::map<std::pair<uint64_t, CService>, std::shared_ptr<PartialBlockData> >::iterator it) {
    uint64_t hash_prefix = it->first.first;
    std::lock_guard<std::mutex> lock(it->second->state_mutex);
    // Note that we do not modify perNodeChunkCount, as it might be "read-only" due to currentlyProcessing
    for (const std::pair<CService, std::pair<uint32_t, uint32_t> >& node : it->second->perNodeChunkCount) {
        std::map<CService, UDPConnectionState>::iterator nodeIt = mapUDPNodes.find(node.first);
        if (nodeIt == mapUDPNodes.end())
            continue;
        std::map<uint64_t, ChunksAvailableSet>::iterator chunks_avail_it = nodeIt->second.chunks_avail.find(hash_prefix);
        if (chunks_avail_it == nodeIt->second.chunks_avail.end())
            continue; // Peer reconnected at some point
        nodeIt->second.chunks_avail.erase(chunks_avail_it);
    }
    return mapPartialBlocks.erase(it);
}

static void RemovePartialBlock(const std::pair<uint64_t, CService>& key) {
    auto it = mapPartialBlocks.find(key);
    if (it != mapPartialBlocks.end())
        RemovePartialBlock(it);
}

static void RemovePartialBlocks(uint64_t hash_prefix) {
    std::map<std::pair<uint64_t, CService>, std::shared_ptr<PartialBlockData> >::iterator it = mapPartialBlocks.lower_bound(std::make_pair(hash_prefix, TRUSTED_PEER_DUMMY));
    while (it != mapPartialBlocks.end() && it->first.first == hash_prefix)
        it = RemovePartialBlock(it);
}

static inline void SendMessageToNode(const UDPMessage& msg, unsigned int length, bool high_prio, uint64_t hash_prefix, std::map<CService, UDPConnectionState>::iterator it) {
    if ((it->second.state & STATE_INIT_COMPLETE) != STATE_INIT_COMPLETE)
        return;
    const auto chunks_avail_it = it->second.chunks_avail.find(hash_prefix);

    bool use_chunks_avail = chunks_avail_it != it->second.chunks_avail.end();
    if (use_chunks_avail) {
        if (chunks_avail_it->second.AreAllAvailable())
            return;

        if ((msg.header.msg_type & UDP_MSG_TYPE_TYPE_MASK) == MSG_TYPE_BLOCK_HEADER) {
            if (chunks_avail_it->second.IsHeaderChunkAvailable(le32toh(msg.msg.block.chunk_id)))
                return;
        } else {
            if (!chunks_avail_it->second.IsBlockDataChunkCountSet())
                chunks_avail_it->second.SetBlockDataChunkCount(DIV_CEIL(le32toh(msg.msg.block.obj_length), sizeof(UDPBlockMessage::data)));
            if (chunks_avail_it->second.IsBlockChunkAvailable(le32toh(msg.msg.block.chunk_id)))
                return;
        }
    }

    SendMessage(msg, length, high_prio, it);

    if (use_chunks_avail) {
        if ((msg.header.msg_type & UDP_MSG_TYPE_TYPE_MASK) == MSG_TYPE_BLOCK_HEADER)
            chunks_avail_it->second.SetHeaderChunkAvailable(le32toh(msg.msg.block.chunk_id));
        else if ((msg.header.msg_type & UDP_MSG_TYPE_TYPE_MASK) == MSG_TYPE_BLOCK_CONTENTS)
            chunks_avail_it->second.SetBlockChunkAvailable(le32toh(msg.msg.block.chunk_id));
    }
}

static void SendMessageToAllNodes(const UDPMessage& msg, unsigned int length, bool high_prio, uint64_t hash_prefix) {
    for (std::map<CService, UDPConnectionState>::iterator it = mapUDPNodes.begin(); it != mapUDPNodes.end(); it++)
        SendMessageToNode(msg, length, high_prio, hash_prefix, it);
}

static void CopyMessageData(UDPMessage& msg, const std::vector<unsigned char>& data, size_t msg_chunks, uint16_t chunk_id) {
    msg.msg.block.chunk_id = htole16(chunk_id);

    size_t msg_size = chunk_id == msg_chunks - 1 ? (data.size() % FEC_CHUNK_SIZE) : sizeof(msg.msg.block.data);
    if (msg_size == 0) msg_size = FEC_CHUNK_SIZE;
    memcpy(msg.msg.block.data, &data[chunk_id * FEC_CHUNK_SIZE], msg_size);
    if (msg_size != sizeof(msg.msg.block.data))
        memset(&msg.msg.block.data[msg_size], 0, sizeof(msg.msg.block.data) - msg_size);
}

/**
 * Send uncoded (non FEC-coded) data chunks to all peers
 *
 * NOTE: algorithm is broken if you use both high_prio_chunks_per_peer and
 * chunk_limit!
 */
static void RelayUncodedChunks(UDPMessage& msg, const std::vector<unsigned char>& data, const size_t high_prio_chunks_per_peer, const uint64_t hash_prefix, const size_t chunk_limit) {
    const size_t msg_chunks = DIV_CEIL(data.size(), FEC_CHUNK_SIZE);

    size_t chunks_sent_per_peer = 0;
    bool high_prio = high_prio_chunks_per_peer;
    for (auto it = mapUDPNodes.begin(); it != mapUDPNodes.end(); it++) {
        auto send_it = it;
        for (uint16_t i = 0; i < msg_chunks && i < chunk_limit; i++) {
            CopyMessageData(msg, data, msg_chunks, i);

            SendMessageToNode(msg, sizeof(UDPMessageHeader) + sizeof(UDPBlockMessage), high_prio, hash_prefix, send_it);
            send_it++;
            if (send_it == mapUDPNodes.end()) {
                send_it = mapUDPNodes.begin();
                chunks_sent_per_peer++;
                if (high_prio && chunks_sent_per_peer >= high_prio_chunks_per_peer) high_prio = false;
            }
        }
    }
}

struct DataFECer {
    size_t fec_chunks;
    std::pair<std::unique_ptr<FECChunkType[]>, std::vector<uint32_t>> fec_data;
    FECEncoder enc;
    DataFECer(const std::vector<unsigned char>& data, size_t fec_chunks_in) :
        fec_chunks(fec_chunks_in),
        fec_data(std::piecewise_construct, std::forward_as_tuple(new FECChunkType[fec_chunks]), std::forward_as_tuple(fec_chunks)),
        enc(&data, &fec_data) {}

    DataFECer(FECDecoder&& decoder, const std::vector<unsigned char>& data, size_t fec_chunks_in) :
        fec_chunks(fec_chunks_in),
        fec_data(std::piecewise_construct, std::forward_as_tuple(new FECChunkType[fec_chunks]), std::forward_as_tuple(fec_chunks)),
        enc(std::move(decoder), &data, &fec_data) {}
};

static void CopyFECData(UDPMessage& msg, DataFECer& fec, size_t array_idx, bool overwrite_chunk = false) {
    assert(fec.enc.BuildChunk(array_idx, overwrite_chunk)); // TODO: Handle errors?
    assert(fec.fec_data.second[array_idx] < (1 << 24));
    msg.msg.block.chunk_id = htole32(fec.fec_data.second[array_idx]);
    memcpy(msg.msg.block.data, &fec.fec_data.first[array_idx], FEC_CHUNK_SIZE);
}

/* Send FEC-coded chunks to all peers */
static void RelayFECedChunks(UDPMessage& msg, DataFECer& fec, const size_t msg_chunks, const size_t high_prio_chunks_per_peer, const uint64_t hash_prefix) {
    assert(fec.fec_chunks > 9);

    size_t chunks_sent_per_peer = 0;
    bool high_prio = high_prio_chunks_per_peer;
    for (auto it = mapUDPNodes.begin(); it != mapUDPNodes.end(); it++) {
        auto send_it = it;
        for (size_t i = 0; i < fec.fec_chunks; i++) {
            CopyFECData(msg, fec, i);

            SendMessageToNode(msg, sizeof(UDPMessageHeader) + sizeof(UDPBlockMessage), high_prio, hash_prefix, send_it);
            send_it++;
            if (send_it == mapUDPNodes.end()) {
                send_it = mapUDPNodes.begin();
                chunks_sent_per_peer++;
                if (high_prio && chunks_sent_per_peer >= high_prio_chunks_per_peer) high_prio = false;
            }
        }
    }
}

static inline void FillCommonMessageHeader(UDPMessage& msg, const uint64_t hash_prefix, uint8_t type, const size_t obj_size) {
    msg.header.msg_type        = type;
    msg.msg.block.hash_prefix  = htole64(hash_prefix);
    msg.msg.block.obj_length   = htole32(obj_size);
}

static inline void FillBlockMessageHeader(UDPMessage& msg, const uint64_t hash_prefix, UDPMessageType type, const size_t obj_size) {
    // First fill in common message elements
    FillCommonMessageHeader(msg, hash_prefix, type | HAVE_BLOCK, obj_size);
}

/**
 * Send FEC-coded and uncoded (original data) chunks to all peers
 *
 * This function processes either header chunks or block chunks, but not
 * both. So it has to be called twice. After completion, all chunks (of the
 * header or block) will be queued up for transmission.
 */
static void RelayChunks(const uint256& blockhash, UDPMessageType type, const std::vector<unsigned char>& data, DataFECer& fec) {
    UDPMessage msg;
    uint64_t hash_prefix = blockhash.GetUint64(0);
    const size_t msg_chunks = DIV_CEIL(data.size(), FEC_CHUNK_SIZE);
    FillBlockMessageHeader(msg, hash_prefix, type, data.size());

    // For header messages, the actual data is more useful.
    // For block contents, the probably generated most chunks from the header + mempool.
    // We send in usefulness-first order
    if (type == MSG_TYPE_BLOCK_HEADER) {
        // Block headers are all high priority for the data itself,
        // and 3 packets of high priority for the FEC, after that if
        // we have block data available it should be sent.
        RelayUncodedChunks(msg, data, std::numeric_limits<size_t>::max(), hash_prefix, std::numeric_limits<size_t>::max());
        RelayFECedChunks(msg, fec, msg_chunks, 3, hash_prefix);
    } else {
        // First 10 FEC chunks are high priority, then everything is
        // low. This should be sufficient to reconstruct many blocks
        // that only missed a handful of chunks, then revert to
        // sending header chunks until we've sent them all.
        RelayFECedChunks(msg, fec, msg_chunks, 10, hash_prefix);

        // We also benchmark sending pre-calced data here to ensure there
        // isn't a lot of overhead here...
        const bool fBench = LogAcceptCategory(BCLog::BENCH);
        std::chrono::steady_clock::time_point start;
        if (fBench)
            start = std::chrono::steady_clock::now();
        RelayUncodedChunks(msg, data, 0, hash_prefix, std::numeric_limits<size_t>::max());
        if (fBench) {
            std::chrono::steady_clock::time_point finished(std::chrono::steady_clock::now());
            LogPrintf("UDP: Sent block data chunks in %lf ms\n", to_millis_double(finished - start));
        }
    }
}

static void SendLimitedDataChunks(const uint256& blockhash, UDPMessageType type, const std::vector<unsigned char>& data) {
    UDPMessage msg;
    uint64_t hash_prefix = blockhash.GetUint64(0);
    FillBlockMessageHeader(msg, hash_prefix, type, data.size());

    RelayUncodedChunks(msg, data, std::numeric_limits<size_t>::max(), hash_prefix, 3); // Send 3 packets to each peer, in RR
}

static boost::thread *process_block_thread = NULL;
void UDPRelayBlock(const CBlock& block) {
    std::chrono::steady_clock::time_point start;
    const bool fBench = LogAcceptCategory(BCLog::BENCH);
    if (fBench)
        start = std::chrono::steady_clock::now();

    uint256 hashBlock(block.GetHash());
    uint64_t hash_prefix = hashBlock.GetUint64(0);
    std::unique_lock<std::recursive_mutex> lock(cs_mapUDPNodes, std::defer_lock);

    if (maybe_have_write_nodes) { // Scope for partial_block_lock and partial_block_ptr
        const std::vector<unsigned char> *chunk_coded_block = NULL;
        bool skipEncode = false;
        std::unique_lock<std::mutex> partial_block_lock;
        std::shared_ptr<PartialBlockData> partial_block_ptr;
        bool inUDPProcess = process_block_thread && boost::this_thread::get_id() == process_block_thread->get_id();
        if (inUDPProcess) {
            lock.lock();

            auto it = mapPartialBlocks.find(std::make_pair(hash_prefix, TRUSTED_PEER_DUMMY));
            if (it != mapPartialBlocks.end() && it->second->currentlyProcessing) {
                partial_block_lock = std::unique_lock<std::mutex>(it->second->state_mutex); // Locked after cs_mapUDPNodes
                if (it->second->block_data.AreChunksAvailable()) {
                    if (fBench)
                        LogPrintf("UDP: Building FEC chunks from decoded block\n");
                    skipEncode = true;
                    partial_block_ptr = it->second;
                    chunk_coded_block = &it->second->block_data.GetCodedBlock();
                }
            }

            // We unlock everything here to let the net thread relay packets,
            // but continue to use data which is theoretically under the locks.
            // This is OK - we get a copy of the shared_ptr and hold it in
            // partial_block_ptr so it wont be destroyed out from under us, and
            // are only using the chunks from PartiallyDownloadedChunkBlock and
            // the decoder, both of which, once available, will never become
            // un-available or be modified by any other thread (due to the
            // currentlyProcessing checks made in the net thread).
            // We should not otherwise be making assumptions about availability of
            // block-related data, but eg the message send functions check for the
            // availability of ChunkAvailableSets prior to access.
            if (partial_block_lock)
                partial_block_lock.unlock();
            lock.unlock();
        }

        std::chrono::steady_clock::time_point initd;
        if (fBench)
            initd = std::chrono::steady_clock::now();

        boost::optional<ChunkCodedBlock> codedBlock;
        CBlockHeaderAndLengthShortTxIDs headerAndIDs(block, true);
        std::vector<unsigned char> header_data;
        header_data.reserve(2500 + 8 * block.vtx.size()); // Rather conservatively high estimate
        VectorOutputStream stream(&header_data, SER_NETWORK, SERIALIZE_TRANSACTION_COMPRESSED | PROTOCOL_VERSION);
        stream << headerAndIDs;

        std::chrono::steady_clock::time_point coded;
        if (fBench)
            coded = std::chrono::steady_clock::now();

        DataFECer header_fecer(header_data, (min_per_node_mbps.load(std::memory_order_relaxed) * 1024 * 1024 / 8 / 1000 / PACKET_SIZE) + 10); // 1ms + 10 chunks of header FEC

        boost::optional<DataFECer> block_fecer;
        size_t data_fec_chunks = 0;
        if (inUDPProcess) {
            // If we're actively receiving UDP packets, go ahead and spend the time to precalculate FEC now,
            // otherwise we want to start getting the header/first block chunks out ASAP
            header_fecer.enc.PrefillChunks();

            if (!skipEncode) {
                codedBlock.emplace(block, headerAndIDs);
                chunk_coded_block = &codedBlock->GetCodedBlock();
            }
            if (!chunk_coded_block->empty()) {
                data_fec_chunks = DIV_CEIL(chunk_coded_block->size(), FEC_CHUNK_SIZE) + 10; //TODO: Pick something different?
                if (skipEncode) {
                    // If we get here, we are currently in the processing thread
                    // and have partial_block_ptr set. Additionally, because
                    // partial_block_ptr->block_data has chunks, the FEC decoder
                    // was initialized and fed FEC/data, meaning even if no FEC
                    // chunks were used to reconstruct the FECDecoder object is
                    // fully primed to be converted to a FECEncoder!
                    block_fecer.emplace(std::move(partial_block_ptr->decoder), *chunk_coded_block, data_fec_chunks);
                } else {
                    block_fecer.emplace(*chunk_coded_block, data_fec_chunks);
                }
                block_fecer->enc.PrefillChunks();
            }
        }

        std::chrono::steady_clock::time_point feced;
        if (fBench)
            feced = std::chrono::steady_clock::now();

        // We do all the expensive calculations before locking cs_mapUDPNodes
        // so that the forward-packets-without-block logic in HandleBlockMessage
        // continues without interruption as long as possible
        if (!lock)
            lock.lock();

        if (mapUDPNodes.empty())
            return;

        if (setBlocksRelayed.count(hash_prefix))
            return;

        RelayChunks(hashBlock, MSG_TYPE_BLOCK_HEADER, header_data, header_fecer);

        std::chrono::steady_clock::time_point header_sent;
        if (fBench)
            header_sent = std::chrono::steady_clock::now();

        if (!inUDPProcess) { // We sent header before calculating any block stuff
            if (!skipEncode) {
                codedBlock.emplace(block, headerAndIDs);
                chunk_coded_block = &codedBlock->GetCodedBlock();
            }

            // Because we need the coded block's size to init block decoding, it
            // is important we get the first block packet out to peers ASAP. Thus,
            // we go ahead and send the first few non-FEC block packets here.
            if (!chunk_coded_block->empty()) {
                data_fec_chunks = DIV_CEIL(chunk_coded_block->size(), FEC_CHUNK_SIZE) + 10; //TODO: Pick something different?
                SendLimitedDataChunks(hashBlock, MSG_TYPE_BLOCK_CONTENTS, *chunk_coded_block);
            }
        }

        std::chrono::steady_clock::time_point block_coded;
        if (fBench)
            block_coded = std::chrono::steady_clock::now();

        if (!inUDPProcess) { // We sent header before calculating any block stuff
            if (!chunk_coded_block->empty()) {
                block_fecer.emplace(*chunk_coded_block, data_fec_chunks);
            }
        }

        std::chrono::steady_clock::time_point block_fec_initd;
        if (fBench)
            block_fec_initd = std::chrono::steady_clock::now();

        // Now (maybe) send the transaction chunks
        if (!chunk_coded_block->empty())
            RelayChunks(hashBlock, MSG_TYPE_BLOCK_CONTENTS, *chunk_coded_block, *block_fecer);

        if (fBench) {
            std::chrono::steady_clock::time_point all_sent(std::chrono::steady_clock::now());
            LogPrintf("UDP: Built all FEC chunks for block %s in %lf %lf %lf %lf %lf %lf %lf ms with %lu header chunks\n", hashBlock.ToString(), to_millis_double(initd - start), to_millis_double(coded - initd), to_millis_double(feced - coded), to_millis_double(header_sent - feced), to_millis_double(block_coded - header_sent), to_millis_double(block_fec_initd - block_coded), to_millis_double(all_sent - block_fec_initd), header_fecer.fec_chunks);
            if (!inUDPProcess)
                LogPrintf("UDP: Block %s had serialized size %lu\n", hashBlock.ToString(), GetSerializeSize(block, SERIALIZE_TRANSACTION_COMPRESSED | PROTOCOL_VERSION));
        } else
            LogPrintf("UDP: Built all FEC chunks for block %s\n", hashBlock.ToString());

        // Destroy partial_block_lock before we RemovePartialBlocks()
    }

    setBlocksRelayed.insert(hash_prefix);
    RemovePartialBlocks(hash_prefix);
}

/**
 * Send txn over one or more messages
 *
 * All txns are sent uncoded (without FEC-coding). Yet, the txns with size
 * yielding more than a single data chunk are still treated as FEC-coded on the
 * receive-end. This is OK, since the coding scheme is systematic. Most likely,
 * such (larger) txns will be encoded by cm256.
 */
void UDPFillMessagesFromTx(const CTransaction& tx, std::vector<UDPMessage>& msgs) {
    const uint256 hash(tx.GetWitnessHash());
    const uint64_t hash_prefix = hash.GetUint64(0);

    std::vector<unsigned char> data;
    VectorOutputStream stream(&data, SER_NETWORK, SERIALIZE_TRANSACTION_COMPRESSED | PROTOCOL_VERSION);
    stream << CTxCompressor(tx);

    const size_t data_chunks = DIV_CEIL(data.size(), FEC_CHUNK_SIZE);
    msgs.resize(data_chunks);

    for (size_t i = 0; i < data_chunks; i++) {
        FillCommonMessageHeader(msgs[i], hash_prefix, MSG_TYPE_TX_CONTENTS, data.size());
        CopyMessageData(msgs[i], data, data_chunks, i);
    }
}

/**
 * Send block through a composition of block header and block data messages
 *
 * Unlike txns, only FEC-coded chunks are sent for block data. The same strategy
 * is applied also for transmission of the block header, although based on a
 * different motivation, explained next.

 * The main factor to consider when choosing between sending uncoded vs. coded
 * is whether the receive-end might know something about the data object in
 * advance. Once a block is advertised by the "block header" message, the
 * receive node tries to prefill the chunk-coded block based on the txns that it
 * already has locally in its mempool. As a result, the receive node typically
 * prefills most of the chunks before the chunk-coded block even starts to
 * arrive. In this scenario, it is advantageous to send only coded chunks. The
 * rationale is that each coded chunk generated by the fountain code (wirehair)
 * brings information about multiple other chunks (it is a XOR of multiple
 * chunks) and, as a result, the receive end tends to fill all gaps faster by
 * processing them. In contrast, uncoded chunks only bring their own
 * information, so the receive node must receive the exact chunks that are
 * missing in order to fill all gaps, which tends to be slower.
 *
 * In contrast, for objects that the receiver knows nothing about, it can be
 * slightly advantageous to send the uncoded chunks first. Since the code is
 * systematic, sending uncoded is equivalent to sending coded chunks with chunk
 * id < N, where N is the number of chunks of the original object. The reason
 * why it could be advantageous is that the decoder can process the "uncoded
 * chunks" faster (there is less computation involved). On the other hand, when
 * uncoded chunks are sent, the disadvantage is that the receive-end cannot
 * efficiently combine multiple incoming streams in order to decode the object
 * faster. When, instead, coded chunks with random chunk ids are sent, the
 * receive node that listens to multiple streams will get different chunks from
 * each stream and, consequently, complete the decoding quicker.
 *
 * So the choice for objects that the receiver doesn't know about in advance
 * depends on which factor matters the most: the longer decoding of coded chunks
 * (with random chunk ids) or the more efficient combination of chunks when
 * receiving from multiple streams. It turns out that the decoding is typically
 * in the order of microseconds. Meanwhile, transmission of chunks depends on
 * link bitrates. In the implementation that follows, we optimize for low
 * bitrate links (such as satellite links), where the bottleneck typically is
 * the transmission delay of chunks. For instance, on a 1 Mbps link, a 1 KByte
 * chunk has a delay of 8 ms, i.e. much larger than the decoding latency. Hence,
 * we choose to send all chunks as FEC-coded, even for the block header.
 *
 * Besides, the block header will essentially always be coded via cm256 (rather
 * than wirehair), due to its smaller size (compared to the chunk-coded
 * block). The difference on decoding duration will be even lower in this case.
 *
 */
void UDPFillMessagesFromBlock(const CBlock& block, std::vector<UDPMessage>& msgs) {
    const uint256 hashBlock(block.GetHash());
    const uint64_t hash_prefix = hashBlock.GetUint64(0);

    /* FIBRE block header */
    CBlockHeaderAndLengthShortTxIDs headerAndIDs(block, true);

    std::vector<unsigned char> header_data;
    header_data.reserve(2500 + 8 * block.vtx.size()); // Rather conservatively high estimate
    VectorOutputStream stream(&header_data, SER_NETWORK, SERIALIZE_TRANSACTION_COMPRESSED | PROTOCOL_VERSION);
    stream << headerAndIDs;
    const size_t header_fec_chunks = DIV_CEIL(header_data.size(), FEC_CHUNK_SIZE) + 2;
    DataFECer header_fecer(header_data, header_fec_chunks);
    /* NOTE: the block header will typically be encoded by cm256, due to its
     * size. Since cm256 is MDS, in principle only the N original chunks are
     * necessary. Nevertheless, since chunks can be lost along the transport
     * link, 2 chunks of overhead are used. */

    /* Block */
    ChunkCodedBlock codedBlock(block, headerAndIDs);
    const std::vector<unsigned char>& chunk_coded_block = codedBlock.GetCodedBlock();
    const size_t block_fec_chunks = DIV_CEIL(chunk_coded_block.size(), FEC_CHUNK_SIZE) + 2;
    /* NOTE: on average wirehair needs about 0.02 chunks of overhead to recover
     * (meaning most often it doesn't need overhead at all). Again, we add two
     * extra chunks here (more than necessary) in order to overcome loss along
     * the transport link. */
    DataFECer block_fecer(chunk_coded_block, block_fec_chunks);

    msgs.resize(header_fec_chunks + block_fec_chunks);

    /* Header chunks */
    for (size_t i = 0; i < header_fec_chunks; i++) {
        FillBlockMessageHeader(msgs[i], hash_prefix, MSG_TYPE_BLOCK_HEADER, header_data.size());
        CopyFECData(msgs[i], header_fecer, i);
    }

    /* Block chunks */
    for (size_t i = 0; i < block_fec_chunks; i++) {
        FillBlockMessageHeader(msgs[i + header_fec_chunks], hash_prefix, MSG_TYPE_BLOCK_CONTENTS, chunk_coded_block.size());
        CopyFECData(msgs[i + header_fec_chunks], block_fecer, i);
    }
}

static std::mutex block_process_mutex;
static std::condition_variable block_process_cv;
static std::atomic_bool block_process_shutdown(false);
static std::vector<std::pair<std::pair<uint64_t, CService>, std::shared_ptr<PartialBlockData> > > block_process_queue;

static void DoBackgroundBlockProcessing(const std::pair<std::pair<uint64_t, CService>, std::shared_ptr<PartialBlockData> >& block_data) {
    // If we just blindly call ProcessNewBlock here, we have a cs_main/cs_mapUDPNodes inversion
    // (actually because fucking P2P code calls everything with cs_main already locked).
    // Instead we pass the processing back to ProcessNewBlockThread without cs_mapUDPNodes
    std::unique_lock<std::mutex> lock(block_process_mutex);
    block_process_queue.emplace_back(block_data);
    lock.unlock();
    block_process_cv.notify_all();
}

static void ProcessBlockThread() {
    const bool fBench = LogAcceptCategory(BCLog::BENCH);

    while (true) {
        std::unique_lock<std::mutex> process_lock(block_process_mutex);
        while (block_process_queue.empty() && !block_process_shutdown)
            block_process_cv.wait(process_lock);
        if (block_process_shutdown)
            return;
        // To avoid vector re-allocation we pop_back, so its secretly a stack, shhhhh, dont tell anyone
        std::pair<std::pair<uint64_t, CService>, std::shared_ptr<PartialBlockData> > process_block = block_process_queue.back();
        CService& node = process_block.first.second;
        PartialBlockData& block = *process_block.second;
        block_process_queue.pop_back();
        process_lock.unlock();

        bool more_work;
        std::unique_lock<std::mutex> lock(block.state_mutex);
        do {
            more_work = false;
            if (block.is_header_processing) {
                std::chrono::steady_clock::time_point decode_start;
                if (fBench)
                    decode_start = std::chrono::steady_clock::now();

                for (uint32_t i = 0; i < DIV_CEIL(block.obj_length, sizeof(UDPBlockMessage::data)); i++) {
                    const void* data_ptr = block.decoder.GetDataPtr(i);
                    assert(data_ptr);
                    memcpy(&block.data_recvd[i * sizeof(UDPBlockMessage::data)], data_ptr, sizeof(UDPBlockMessage::data));
                }

                std::chrono::steady_clock::time_point data_copied;
                if (fBench)
                    data_copied = std::chrono::steady_clock::now();

                CBlockHeaderAndLengthShortTxIDs header;
                try {
                    VectorInputStream stream(&block.data_recvd, SER_NETWORK, SERIALIZE_TRANSACTION_COMPRESSED | PROTOCOL_VERSION);
                    stream >> header;
                } catch (std::ios_base::failure& e) {
                    lock.unlock();
                    std::lock_guard<std::recursive_mutex> udpNodesLock(cs_mapUDPNodes);
                    if (node == TRUSTED_PEER_DUMMY)
                        LogPrintf("UDP: Failed to decode received header and short txids from trusted peer(s), check your trusted peers are behaving well.\n");
                    else {
                        LogPrintf("UDP: Failed to decode received header and short txids from %s, disconnecting\n", node.ToString());
                        const auto it = mapUDPNodes.find(node);
                        if (it != mapUDPNodes.end())
                            DisconnectNode(it);
                    }
                    break;
                }
                std::chrono::steady_clock::time_point header_deserialized;
                if (fBench)
                    header_deserialized = std::chrono::steady_clock::now();

                ReadStatus decode_status = block.ProvideHeaderData(header);
                if (decode_status != READ_STATUS_OK) {
                    lock.unlock();
                    std::lock_guard<std::recursive_mutex> udpNodesLock(cs_mapUDPNodes);
                    if (decode_status == READ_STATUS_INVALID) {
                        if (node == TRUSTED_PEER_DUMMY)
                            LogPrintf("UDP: Got invalid header and short txids from trusted peer(s), check your trusted peers are behaving well.\n");
                        else {
                            LogPrintf("UDP: Got invalid header and short txids from %s, disconnecting\n", node.ToString());
                            const auto it = mapUDPNodes.find(node);
                            if (it != mapUDPNodes.end())
                                DisconnectNode(it);
                        }
                    } else
                        LogPrintf("UDP: Failed to read header and short txids\n");

                    // Dont remove the block, let it time out...
                    break;
                }

                if (block.block_data.IsBlockAvailable())
                    block.is_decodeable.store(true, std::memory_order_release);
                block.is_header_processing.store(false, std::memory_order_release);

                if (block.is_decodeable.load(std::memory_order_acquire))
                    more_work = true;
                else
                    lock.unlock();

                if (fBench) {
                    std::chrono::steady_clock::time_point header_provided(std::chrono::steady_clock::now());
                    LogPrintf("UDP: Got full header and shorttxids from %s in %lf %lf %lf ms\n", block.nodeHeaderRecvd.ToString(), to_millis_double(data_copied - decode_start), to_millis_double(header_deserialized - data_copied), to_millis_double(header_provided - header_deserialized));
                } else
                    LogPrintf("UDP: Got full header and shorttxids from %s\n", block.nodeHeaderRecvd.ToString());
            } else if (block.is_decodeable || block.block_data.IsBlockAvailable()) {
                if (block.currentlyProcessing) {
                    // We often duplicatively schedule DoBackgroundBlockProcessing,
                    // but we do not do anything to avoid duplicate
                    // final-processing. Thus, we have to check if we have already
                    // done final processing by checking currentlyProcessing (which
                    // is never un-set after we set it).
                    break;
                }
                block.currentlyProcessing = true;
                std::chrono::steady_clock::time_point reconstruct_start;
                if (fBench)
                    reconstruct_start = std::chrono::steady_clock::now();

                if (!block.block_data.IsBlockAvailable()) {
                    block.ReconstructBlockFromDecoder();
                    assert(block.block_data.IsBlockAvailable());
                }

                std::chrono::steady_clock::time_point fec_reconstruct_finished;
                if (fBench)
                    fec_reconstruct_finished = std::chrono::steady_clock::now();

                ReadStatus status = block.block_data.FinalizeBlock();

                std::chrono::steady_clock::time_point block_finalized;
                if (fBench)
                    block_finalized = std::chrono::steady_clock::now();

                if (status != READ_STATUS_OK) {
                    lock.unlock();
                    std::lock_guard<std::recursive_mutex> udpNodesLock(cs_mapUDPNodes);

                    if (status == READ_STATUS_INVALID) {
                        if (node == TRUSTED_PEER_DUMMY)
                            LogPrintf("UDP: Unable to decode block from trusted peer(s), check your trusted peers are behaving well.\n");
                        else {
                            const auto it = mapUDPNodes.find(node);
                            if (it != mapUDPNodes.end())
                                DisconnectNode(it);
                        }
                    }
                    setBlocksReceived.insert(process_block.first);
                    RemovePartialBlock(process_block.first);
                    break;
                } else {
                    std::shared_ptr<const CBlock> pdecoded_block = block.block_data.GetBlock();
                    const CBlock& decoded_block = *pdecoded_block;
                    if (fBench) {
                        uint32_t total_chunks_recvd = 0, total_chunks_used = 0;
                        std::map<CService, std::pair<uint32_t, uint32_t> >& chunksProvidedByNode = block.perNodeChunkCount;
                        for (const std::pair<CService, std::pair<uint32_t, uint32_t> >& provider : chunksProvidedByNode) {
                            total_chunks_recvd += provider.second.second;
                            total_chunks_used += provider.second.first;
                        }
                        /* NOTE: the chunk count printed next is not necessarily
                         * accurate. It reflects the count up to when the block
                         * is decoded. However, further chunks may still be
                         * received after the block is decoded. The count kept
                         * at `mapChunkCount` is more reliable. */
                        LogPrintf("UDP: Block %s reconstructed from %s with %u chunks in %lf ms (%u recvd from %u peers)\n", decoded_block.GetHash().ToString(), block.nodeHeaderRecvd.ToString(), total_chunks_used, to_millis_double(std::chrono::steady_clock::now() - block.timeHeaderRecvd), total_chunks_recvd, chunksProvidedByNode.size());
                        for (const std::pair<CService, std::pair<uint32_t, uint32_t> >& provider : chunksProvidedByNode)
                            LogPrintf("UDP:    %u/%u used from %s\n", provider.second.first, provider.second.second, provider.first.ToString());
                    }

                    lock.unlock();

                    std::chrono::steady_clock::time_point process_start;
                    if (fBench)
                        process_start = std::chrono::steady_clock::now();

                    bool fNewBlock;
                    if (!ProcessNewBlock(Params(), pdecoded_block, false, &fNewBlock)) {
                        bool have_prev, save_block_for_later;
                        {
                            LOCK(cs_main);
                            have_prev = BlockIndex().count(pdecoded_block->hashPrevBlock);

                            if (have_prev) {
                                save_block_for_later = false;
                            } else {
                               /* Only save out-of-order blocks received
                                * through a one-way multicast service
                                * Only save blocks that are at least minimally valid */
                                CValidationState state;
                                save_block_for_later = IsMulticastRxNode(node)
                                    && CheckBlock(*pdecoded_block, state, Params().GetConsensus());
                            }
                        }
                        if (save_block_for_later) {
                            save_block_for_later = StoreOoOBlock(Params(), pdecoded_block);
                        }
                        LogPrintf("UDP: Failed to decode block %s\n", decoded_block.GetHash().ToString());
                        std::lock_guard<std::recursive_mutex> udpNodesLock(cs_mapUDPNodes);
                        if (have_prev || save_block_for_later) {
                            setBlocksReceived.insert(process_block.first);
                        } else {
                            // Allow re-downloading again later, useful for local backfill downloads
                            setBlocksReceived.erase(process_block.first);
                        }
                        RemovePartialBlock(process_block.first);
                        break; // Probably a tx collision generating merkle-tree errors
                    }
                    if (fBench) {
                        LogPrintf("UDP: Final block processing for %s took %lf %lf %lf %lf ms (new: %d)\n", decoded_block.GetHash().ToString(), to_millis_double(fec_reconstruct_finished - reconstruct_start), to_millis_double(block_finalized - fec_reconstruct_finished), to_millis_double(process_start - block_finalized), to_millis_double(std::chrono::steady_clock::now() - process_start), fNewBlock);
                        if (fNewBlock) {
                            LogPrintf("UDP: Block %s had serialized size %lu\n", decoded_block.GetHash().ToString(), GetSerializeSize(decoded_block, SERIALIZE_TRANSACTION_COMPRESSED | PROTOCOL_VERSION));
                        }
                    }

                    std::lock_guard<std::recursive_mutex> udpNodesLock(cs_mapUDPNodes);
                    setBlocksReceived.insert(process_block.first);
                    RemovePartialBlocks(process_block.first.first); // Ensure we remove even if we didnt UDPRelayBlock()
                }
            } else if (!block.in_header && block.initialized) {
                uint32_t mempool_provided_chunks = 0;
                uint32_t total_chunk_count = 0;
                uint256 blockHash;
                bool fDone = block.block_data.IsIterativeFillDone();
                while (!fDone) {
                    size_t firstChunkProcessed;
                    if (!lock)
                        lock.lock();
                    if (!total_chunk_count) {
                        total_chunk_count = block.block_data.GetChunkCount();
                        blockHash = block.block_data.GetBlockHash();
                    }
                    ReadStatus res = block.block_data.DoIterativeFill(firstChunkProcessed);
                    if (res != READ_STATUS_OK) {
                        lock.unlock();
                        std::lock_guard<std::recursive_mutex> udpNodesLock(cs_mapUDPNodes);
                        if (res == READ_STATUS_INVALID) {
                            if (node == TRUSTED_PEER_DUMMY)
                                LogPrintf("UDP: Unable to process mempool for block %s from trusted peer(s), check your trusted peers are behaving well.\n", blockHash.ToString());
                            else {
                                LogPrintf("UDP: Unable to process mempool for block %s from %s, disconnecting\n", blockHash.ToString(), node.ToString());
                                const auto it = mapUDPNodes.find(node);
                                if (it != mapUDPNodes.end())
                                    DisconnectNode(it);
                            }
                        } else
                            LogPrintf("UDP: Unable to process mempool for block %s, dropping block\n", blockHash.ToString());
                        setBlocksReceived.insert(process_block.first);
                        RemovePartialBlock(process_block.first);
                        break;
                    } else {
                        while (firstChunkProcessed < total_chunk_count && block.block_data.IsChunkAvailable(firstChunkProcessed)) {
                            if (!block.decoder.HasChunk(firstChunkProcessed)) {
                                block.decoder.ProvideChunk(block.block_data.GetChunk(firstChunkProcessed), firstChunkProcessed);
                                mempool_provided_chunks++;
                            }
                            firstChunkProcessed++;
                        }

                        if (block.decoder.DecodeReady() || block.block_data.IsBlockAvailable()) {
                            block.is_decodeable = true;
                            more_work = true;
                            break;
                        }
                    }
                    fDone = block.block_data.IsIterativeFillDone();
                    if (!fDone && block.packet_awaiting_lock.load(std::memory_order_acquire)) {
                        lock.unlock();
                        std::this_thread::yield();
                    }
                }
                if (lock && !more_work)
                    lock.unlock();
                LogPrintf("UDP: Initialized block %s with %ld/%ld mempool-provided chunks (or more)\n", blockHash.ToString(), mempool_provided_chunks, total_chunk_count);
            }
        } while (more_work);
    }
}

void BlockRecvInit() {
    process_block_thread = new boost::thread(boost::bind(&TraceThread<void (*)()>, "udpprocess", &ProcessBlockThread));
}

void BlockRecvShutdown() {
    if (process_block_thread) {
        block_process_shutdown = true;
        block_process_cv.notify_all();
        process_block_thread->join();
        delete process_block_thread;
        process_block_thread = NULL;
    }
}

// TODO: Use the one from net_processing (with appropriate lock-free-ness)
static std::vector<std::pair<uint256, CTransactionRef>> udpnet_dummy_extra_txn;
ReadStatus PartialBlockData::ProvideHeaderData(const CBlockHeaderAndLengthShortTxIDs& header) {
    assert(in_header);
    in_header = false;
    initialized = false;
    return block_data.InitData(header, udpnet_dummy_extra_txn);
}

bool PartialBlockData::Init(const UDPMessage& msg) {
    assert((msg.header.msg_type & UDP_MSG_TYPE_TYPE_MASK) == MSG_TYPE_BLOCK_HEADER || (msg.header.msg_type & UDP_MSG_TYPE_TYPE_MASK) == MSG_TYPE_BLOCK_CONTENTS);
    obj_length  = msg.msg.block.obj_length;
    if (obj_length > MAX_BLOCK_SERIALIZED_SIZE * MAX_CHUNK_CODED_BLOCK_SIZE_FACTOR)
        return false;
    if ((msg.header.msg_type & UDP_MSG_TYPE_TYPE_MASK) == MSG_TYPE_BLOCK_HEADER)
        data_recvd.resize(DIV_CEIL(obj_length, sizeof(UDPBlockMessage::data)) * sizeof(UDPBlockMessage::data));
    decoder = FECDecoder(obj_length);
    initialized = true;
    return true;
}

PartialBlockData::PartialBlockData(const CService& node, const UDPMessage& msg, const std::chrono::steady_clock::time_point& packet_recv) :
        timeHeaderRecvd(packet_recv), nodeHeaderRecvd(node),
        in_header(true), initialized(false),
        is_decodeable(false), is_header_processing(false),
        currentlyProcessing(false), block_data(&mempool)
    { assert(Init(msg)); }

void PartialBlockData::ReconstructBlockFromDecoder() {
    assert(decoder.DecodeReady());

    for (uint32_t i = 0; i < DIV_CEIL(obj_length, sizeof(UDPBlockMessage::data)); i++) {
        if (!block_data.IsChunkAvailable(i)) {
            const void* data_ptr = decoder.GetDataPtr(i);
            assert(data_ptr);
            memcpy(block_data.GetChunk(i), data_ptr, sizeof(UDPBlockMessage::data));
            block_data.MarkChunkAvailable(i);
        }
    }

    assert(block_data.IsBlockAvailable());
};

static void BlockMsgHToLE(UDPMessage& msg) {
    msg.msg.block.hash_prefix = htole64(msg.msg.block.hash_prefix);
    msg.msg.block.obj_length  = htole32(msg.msg.block.obj_length);
    msg.msg.block.chunk_id    = htole32(msg.msg.block.chunk_id);
}

static bool HandleTx(UDPMessage& msg, size_t length, const CService& node, UDPConnectionState& state) {
    if (msg.msg.block.obj_length > 400000) {
        LogPrintf("UDP: Got massive tx obj_length of %u\n", msg.msg.block.obj_length);
        return false;
    }

    if (state.tx_in_flight_hash_prefix != msg.msg.block.hash_prefix) {
        state.tx_in_flight_hash_prefix = msg.msg.block.hash_prefix;
        state.tx_in_flight_msg_size    = msg.msg.block.obj_length;
        state.tx_in_flight.reset(new FECDecoder(msg.msg.block.obj_length));
    }

    if (!state.tx_in_flight) return true; // Already finished decode

    if (state.tx_in_flight_msg_size != msg.msg.block.obj_length) {
        LogPrintf("UDP: Got inconsistent object length for tx %lu\n", msg.msg.block.hash_prefix);
        return true;
    }

    assert(!state.tx_in_flight->DecodeReady());

    if (!state.tx_in_flight->ProvideChunk(msg.msg.block.data, msg.msg.block.chunk_id)) {
        // Bad chunk id, maybe FEC is upset? Don't disconnect in case it can be random
        LogPrintf("UDP: FEC chunk decode failed for chunk %d from tx %lu from %s\n", msg.msg.block.chunk_id, msg.msg.block.hash_prefix, node.ToString());
        return true;
    }

    if (state.tx_in_flight->DecodeReady()) {
        std::vector<unsigned char> tx_data(msg.msg.block.obj_length);

        for (size_t i = 0; i < DIV_CEIL(tx_data.size(), FEC_CHUNK_SIZE); i++) {
            const void* chunk = state.tx_in_flight->GetDataPtr(i);
            assert(chunk);
            memcpy(tx_data.data() + i * FEC_CHUNK_SIZE, chunk, std::min(tx_data.size() - i * FEC_CHUNK_SIZE, (size_t)FEC_CHUNK_SIZE));
        }

        try {
            VectorInputStream stream(&tx_data, SER_NETWORK, SERIALIZE_TRANSACTION_COMPRESSED | PROTOCOL_VERSION);
            CTransactionRef tx;
            stream >> CTxCompressor(tx);
            LOCK(cs_main);
            CValidationState state;
            AcceptToMemoryPool(mempool, state, tx, nullptr, nullptr, false, 0);
        } catch (std::exception& e) {
            LogPrintf("UDP: Tx decode failed for tx %lu from %s: %s\n", msg.msg.block.hash_prefix, node.ToString(), e.what());
        }

        state.tx_in_flight.reset();
    }

    return true;
}

/* Print chunk stats
 *
 * Print whenever the block hash being delivered by the peer changes, i.e. when
 * the peer starts transmitting another block. To prevent an anticipated print
 * in case the peer is multiplexing streams with distinct blocks, check also
 * that sufficient time has elapsed since last chunk reception of the
 * block. Alternatively, if an even longer duration has elapsed, we are probably
 * not going to receive chunks of the block anymore, so print the stats.
 */
static void printChunkStats(const uint64_t hash_prefix, const CService& peer,
                            const std::map<std::pair<uint64_t, CService>, BlockChunkCount>::iterator& currentIt) {
    std::chrono::steady_clock::time_point t_now(std::chrono::steady_clock::now());
    for (auto chunkCountIt = mapChunkCount.cbegin(); chunkCountIt != mapChunkCount.cend();)
    {
        /* Don't process the map entry that is currently being processed by the
         * caller. Otherwise, it is possible that the entry is removed from the
         * map and the caller ends up with an invalidated iterator.  */
        if (chunkCountIt == currentIt) {
            ++chunkCountIt;
            continue;
        }

        double elapsed_since_last_rx = to_millis_double(t_now - chunkCountIt->second.t_last);
        if (elapsed_since_last_rx > 5000.0 ||
            (chunkCountIt->first.first != hash_prefix &&
             chunkCountIt->first.second == peer &&
             elapsed_since_last_rx > 2000.0)) {
            double dec_duration = to_millis_double(chunkCountIt->second.t_decode -
                                                   chunkCountIt->second.t_first);
            double tot_duration = to_millis_double(chunkCountIt->second.t_last -
                                                   chunkCountIt->second.t_first);

            LogPrint(BCLog::FEC, "FEC: Chunk count for block id %lu from %s:\n",
                     chunkCountIt->first.first,
                     chunkCountIt->first.second.ToString());
            LogPrint(BCLog::FEC, "    Total chunks:  %4d used / %4d rcvd\n",
                     (chunkCountIt->second.data_used +
                      chunkCountIt->second.header_used),
                     (chunkCountIt->second.data_rcvd +
                      chunkCountIt->second.header_rcvd));
            LogPrint(BCLog::FEC, "    Header chunks: %4d used / %4d rcvd\tDecodable after: %d\n",
                     chunkCountIt->second.header_used,
                     chunkCountIt->second.header_rcvd,
                     chunkCountIt->second.header_to_decode);
            LogPrint(BCLog::FEC, "    Data chunks:   %4d used / %4d rcvd\tDecodable after: %d\n",
                     chunkCountIt->second.data_used,
                     chunkCountIt->second.data_rcvd,
                     chunkCountIt->second.data_to_decode);
            LogPrint(BCLog::FEC, "    Duration: %.2f ms total\t%.2f ms until decodable\n",
                     tot_duration, dec_duration);

            chunkCountIt = mapChunkCount.erase(chunkCountIt);
        } else
            ++chunkCountIt;
    }
}

bool HandleBlockTxMessage(UDPMessage& msg, size_t length, const CService& node, UDPConnectionState& state, const std::chrono::steady_clock::time_point& packet_process_start) {
    //TODO: There are way too many damn tree lookups here...either cut them down or increase parallelism
    const bool fBench = LogAcceptCategory(BCLog::BENCH);
    const bool debugFec = LogAcceptCategory(BCLog::FEC);
    std::chrono::steady_clock::time_point start;
    if (fBench)
        start = std::chrono::steady_clock::now();

    assert((msg.header.msg_type & UDP_MSG_TYPE_TYPE_MASK) == MSG_TYPE_BLOCK_HEADER || (msg.header.msg_type & UDP_MSG_TYPE_TYPE_MASK) == MSG_TYPE_BLOCK_CONTENTS || (msg.header.msg_type & UDP_MSG_TYPE_TYPE_MASK) == MSG_TYPE_TX_CONTENTS);

    if (length != sizeof(UDPMessageHeader) + sizeof(UDPBlockMessage)) {
        LogPrintf("UDP: Got invalidly-sized block message from %s\n", node.ToString());
        return false;
    }

    msg.msg.block.hash_prefix = le64toh(msg.msg.block.hash_prefix);
    msg.msg.block.obj_length  = le32toh(msg.msg.block.obj_length);
    msg.msg.block.chunk_id    = le32toh(msg.msg.block.chunk_id);

    if ((msg.header.msg_type & UDP_MSG_TYPE_TYPE_MASK) == MSG_TYPE_TX_CONTENTS)
        return HandleTx(msg, length, node, state);

    const uint64_t hash_prefix = msg.msg.block.hash_prefix; // Need a reference in a few places, but its packed, so we can't have one directly
    CService peer              = state.connection.fTrusted ? TRUSTED_PEER_DUMMY : node;
    const std::pair<uint64_t, CService> hash_peer_pair = std::make_pair(hash_prefix, peer);

    if (msg.msg.block.obj_length > MAX_BLOCK_SERIALIZED_SIZE * MAX_CHUNK_CODED_BLOCK_SIZE_FACTOR) {
        LogPrintf("UDP: Got massive obj_length of %u\n", msg.msg.block.obj_length);
        return false;
    }

    /* Track all received chunks */
    std::map<std::pair<uint64_t, CService>, BlockChunkCount>::iterator mapChunkCountIt;
    if (debugFec) {
        std::chrono::steady_clock::time_point t_chunk_rcvd(std::chrono::steady_clock::now());

        mapChunkCountIt = mapChunkCount.emplace(hash_peer_pair, BlockChunkCount{0, 0, 0, 0, 0, 0, t_chunk_rcvd, t_chunk_rcvd, t_chunk_rcvd}).first;

        if ((msg.header.msg_type & UDP_MSG_TYPE_TYPE_MASK) == MSG_TYPE_BLOCK_HEADER)
            mapChunkCountIt->second.header_rcvd++;
        else
            mapChunkCountIt->second.data_rcvd++;

        mapChunkCountIt->second.t_last = t_chunk_rcvd;

        printChunkStats(hash_prefix, peer, mapChunkCountIt);
    }

    if (setBlocksRelayed.count(msg.msg.block.hash_prefix) || setBlocksReceived.count(hash_peer_pair))
        return true;

    std::map<uint64_t, ChunksAvailableSet>::iterator chunks_avail_it = state.chunks_avail.find(msg.msg.block.hash_prefix);

    if (chunks_avail_it == state.chunks_avail.end()) {
        if ((msg.header.msg_type & UDP_MSG_TYPE_TYPE_MASK) == MSG_TYPE_BLOCK_HEADER) {
            if (state.chunks_avail.size() > 1 && !state.connection.fTrusted) {
                // Non-trusted nodes can only be forwarding up to 2 blocks at a time
                assert(state.chunks_avail.size() == 2);
                auto first_partial_block_it  = mapPartialBlocks.find(std::make_pair(state.chunks_avail. begin()->first, node));
                assert(first_partial_block_it != mapPartialBlocks.end());
                auto second_partial_block_it = mapPartialBlocks.find(std::make_pair(state.chunks_avail.rbegin()->first, node));
                assert(second_partial_block_it != mapPartialBlocks.end());
                if (first_partial_block_it->second->timeHeaderRecvd < second_partial_block_it->second->timeHeaderRecvd) {
                    state.chunks_avail.erase(first_partial_block_it->first.first);
                    mapPartialBlocks.erase(first_partial_block_it);
                } else {
                    state.chunks_avail.erase(second_partial_block_it->first.first);
                    mapPartialBlocks.erase(second_partial_block_it);
                }
            }
            // Once we add to chunks_avail, we MUST add to mapPartialBlocks->second->perNodeChunkCount, or we will leak memory
            bool they_have_block = msg.header.msg_type & HAVE_BLOCK;
            size_t header_data_chunks = DIV_CEIL(msg.msg.block.obj_length, sizeof(UDPBlockMessage::data));
            chunks_avail_it = state.chunks_avail.emplace(std::piecewise_construct,
                                                         std::forward_as_tuple(hash_prefix),
                                                         std::forward_as_tuple(they_have_block, header_data_chunks)
                                                 ).first;
        } else // Probably stale (ie we just finished reconstructing
            return true;
    }

    if (msg.header.msg_type & HAVE_BLOCK)
        chunks_avail_it->second.SetAllAvailable();
    else {
        // By calling Set*ChunkAvailable before SendMessageToNode's
        // SetHeaderDataAndFECChunkCount call, we will miss the first block packet we
        // receive and re-send that in UDPRelayBlock...this is OK because we'll save
        // more by doing this before the during-process relay below
        if ((msg.header.msg_type & UDP_MSG_TYPE_TYPE_MASK) == MSG_TYPE_BLOCK_HEADER)
            chunks_avail_it->second.SetHeaderChunkAvailable(msg.msg.block.chunk_id);
        else {
            if (!chunks_avail_it->second.IsBlockDataChunkCountSet())
                chunks_avail_it->second.SetBlockDataChunkCount(DIV_CEIL(msg.msg.block.obj_length, sizeof(UDPBlockMessage::data)));
            chunks_avail_it->second.SetBlockChunkAvailable(msg.msg.block.chunk_id);
        }
    }

    bool new_block = false;
    std::map<std::pair<uint64_t, CService>, std::shared_ptr<PartialBlockData> >::iterator it = mapPartialBlocks.find(hash_peer_pair);
    if (it == mapPartialBlocks.end()) {
        if ((msg.header.msg_type & UDP_MSG_TYPE_TYPE_MASK) == MSG_TYPE_BLOCK_HEADER)
            it = mapPartialBlocks.insert(std::make_pair(std::make_pair(hash_prefix, state.connection.fTrusted ? TRUSTED_PEER_DUMMY : node), std::make_shared<PartialBlockData>(node, msg, packet_process_start))).first;
        else // Probably stale (ie we just finished reconstructing)
            return true;
        new_block = true;
    }
    PartialBlockData& block = *it->second;

    std::chrono::steady_clock::time_point maps_scanned;
    if (fBench)
        maps_scanned = std::chrono::steady_clock::now();

    std::unique_lock<std::mutex> block_lock(block.state_mutex, std::try_to_lock);

    if (block.is_decodeable || block.currentlyProcessing || block.is_header_processing ||
        ((msg.header.msg_type & UDP_MSG_TYPE_TYPE_MASK) == MSG_TYPE_BLOCK_HEADER && !block.in_header)) {
        // It takes quite some time to decode the block.
        // Further, its good to check some things like if this packet is for
        // header after we've gotten the header to avoid taking the block_lock
        // and possibly interrupting background fill.
        // Thus, while the block is processing in ProcessNewBlockThread, we
        // continue forwarding chunks we received from trusted peers
        // Note that we will also drop block body packets here while processing
        // the header, sadly isnt much we can do about that (unless we were to
        // queue them, but most of the packets we'll drop here are header FEC
        // anyway, so not much use in doing so).
        if (state.connection.fTrusted) {
            BlockMsgHToLE(msg);
            if (block.is_decodeable || block.currentlyProcessing)
                msg.header.msg_type |= HAVE_BLOCK;
            else
                msg.header.msg_type &= ~HAVE_BLOCK;
            // We didn't need this chunk, call it low priority assuming our
            // peers didn't as well
            SendMessageToAllNodes(msg, length, false, hash_prefix);
        }
        return true;
    }

    if (!block_lock) {
        block.packet_awaiting_lock.store(true, std::memory_order_release);
        block_lock.lock();
        block.packet_awaiting_lock.store(false, std::memory_order_release);
    }

    // is_decodeable || is_headerProcessing must come before any chunk-accessors in block.block_data
    if (block.is_decodeable || block.currentlyProcessing || block.is_header_processing)
        return true;

    std::map<CService, std::pair<uint32_t, uint32_t>>::iterator perNodeChunkCountIt =
            block.perNodeChunkCount.insert(std::make_pair(node, std::make_pair(0, 0))).first;
    perNodeChunkCountIt->second.second++;

    if ((msg.header.msg_type & UDP_MSG_TYPE_TYPE_MASK) == MSG_TYPE_BLOCK_HEADER && !block.in_header) {
        if (state.connection.fTrusted) {
            // Keep forwarding on header packets to our peers to make sure they
            // get the whole header.
            BlockMsgHToLE(msg);
            msg.header.msg_type &= ~HAVE_BLOCK;
            // We didn't need this chunk, call it low priority assuming our
            // peers didn't as well
            SendMessageToAllNodes(msg, length, false, hash_prefix);
        }
        return true;
    }
    if ((msg.header.msg_type & UDP_MSG_TYPE_TYPE_MASK) == MSG_TYPE_BLOCK_CONTENTS && block.in_header) {
        // Either we're getting packets out of order and wasting this packet,
        // or we didnt get enough header and will fail download anyway
        return true;
    }

    if ((msg.header.msg_type & UDP_MSG_TYPE_TYPE_MASK) == MSG_TYPE_BLOCK_CONTENTS && !block.initialized) {
        if (!block.Init(msg)) {
            LogPrintf("UDP: Got block contents that couldn't match header for block id %lu\n", msg.msg.block.hash_prefix);
            return true;
        }
        DoBackgroundBlockProcessing(*it); // Kick off mempool scan (waits on us to unlock block_lock)
    }

    if (msg.msg.block.obj_length  != block.obj_length) {
        // Duplicate hash_prefix or bad trusted peer
        LogPrintf("UDP: Got wrong obj_length/chunsk_sent for block id %lu from peer %s! Check your trusted peers are behaving well\n", msg.msg.block.hash_prefix, node.ToString());
        return true;
    }

    if (block.decoder.HasChunk(msg.msg.block.chunk_id))
        return true;

    if ((msg.header.msg_type & UDP_MSG_TYPE_TYPE_MASK) == MSG_TYPE_BLOCK_CONTENTS &&
            msg.msg.block.chunk_id < block.block_data.GetChunkCount()) {
        assert(!block.block_data.IsChunkAvailable(msg.msg.block.chunk_id)); // HasChunk should have returned false, then
        memcpy(block.block_data.GetChunk(msg.msg.block.chunk_id), msg.msg.block.data, sizeof(UDPBlockMessage::data));
        block.block_data.MarkChunkAvailable(msg.msg.block.chunk_id);
    }
    //TODO: Also pre-copy header data into data_recvd here, if its a non-FEC chunk

    if (!block.decoder.ProvideChunk(msg.msg.block.data, msg.msg.block.chunk_id)) {
        // Bad chunk id, maybe FEC is upset? Don't disconnect in case it can be random
        LogPrintf("UDP: FEC chunk decode failed for chunk %d from block %lu from %s\n", msg.msg.block.chunk_id, msg.msg.block.hash_prefix, node.ToString());
        return true;
    }

    std::chrono::steady_clock::time_point chunks_processed;
    if (fBench)
        chunks_processed = std::chrono::steady_clock::now();

    // Keep track of chunks that are actually used for decoding
    perNodeChunkCountIt->second.first++;
    if (debugFec) {
        if ((msg.header.msg_type & UDP_MSG_TYPE_TYPE_MASK) == MSG_TYPE_BLOCK_HEADER)
            mapChunkCountIt->second.header_used++;
        else
            mapChunkCountIt->second.data_used++;
    }

    if (state.connection.fTrusted) {
        BlockMsgHToLE(msg);
        msg.header.msg_type &= ~HAVE_BLOCK;
        // We needed this chunk, call it high priority assuming our
        // peers will as well
        SendMessageToAllNodes(msg, length, true, hash_prefix);
    }

    if (block.decoder.DecodeReady()) {
        if (block.in_header)
            block.is_header_processing = true;
        else
            block.is_decodeable = true;

        if (debugFec) {
            if (block.in_header)
                mapChunkCountIt->second.header_to_decode = mapChunkCountIt->second.header_rcvd;
            else {
                mapChunkCountIt->second.data_to_decode = mapChunkCountIt->second.data_rcvd;
                mapChunkCountIt->second.t_decode       = std::chrono::steady_clock::now();
            }
        }

        // We do not RemovePartialBlock as we want ChunkAvailableSets to be there when UDPRelayBlock gets called
        // from inside ProcessBlockThread, so after we notify the ProcessNewBlockThread we cannot access block.
        block_lock.unlock();
        DoBackgroundBlockProcessing(*it); // Decode block and call ProcessNewBlock

        if (block.is_decodeable) {
            // Make sure we throw out any future packets for this block
            setBlocksReceived.insert(hash_peer_pair);
        }
    }

    if (fBench && new_block) {
        std::chrono::steady_clock::time_point finished(std::chrono::steady_clock::now());
        LogPrintf("UDP: Processed first block header chunk in %lf %lf %lf %lf\n", to_millis_double(start - packet_process_start), to_millis_double(maps_scanned - start), to_millis_double(chunks_processed - maps_scanned), to_millis_double(finished - chunks_processed));
    }

    return true;
}

void ProcessDownloadTimerEvents() {
    std::unique_lock<std::recursive_mutex> lock(cs_mapUDPNodes);
    for (auto it = mapPartialBlocks.begin(); it != mapPartialBlocks.end();) {
        if (to_millis_double(std::chrono::steady_clock::now() - it->second->timeHeaderRecvd) > 1000 * 60 * 60 * 24) // 1 day
            it = RemovePartialBlock(it);
        else
            it++;
    }
    //TODO: Prune setBlocksRelayed and setBlocksReceived to keep lookups fast?
}
