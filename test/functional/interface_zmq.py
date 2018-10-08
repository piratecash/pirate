#!/usr/bin/env python3
# Copyright (c) 2015-2016 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the ZMQ notification interface."""
import struct

from codecs import encode

from test_framework.test_framework import (
    BitcoinTestFramework, skip_if_no_bitcoind_zmq, skip_if_no_py3_zmq)
from test_framework.messages import cosantahash
from test_framework.util import (assert_equal,
                                 hash256,
                                 )

ADDRESS = "tcp://127.0.0.1:28332"

def dashhash_helper(b):
    return encode(cosantahash(b)[::-1], 'hex_codec').decode('ascii')

class ZMQSubscriber:
    def __init__(self, socket, topic):
        self.sequence = 0
        self.socket = socket
        self.topic = topic

        import zmq
        self.socket.setsockopt(zmq.SUBSCRIBE, self.topic)

    def receive(self):
        topic, body, seq = self.socket.recv_multipart()
        # Topic should match the subscriber topic.
        assert_equal(topic, self.topic)
        # Sequence should be incremental.
        assert_equal(struct.unpack('<I', seq)[-1], self.sequence)
        self.sequence += 1
        return body


class ZMQTest (BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2

    def setup_nodes(self):
        skip_if_no_py3_zmq()
        skip_if_no_bitcoind_zmq(self)

        import zmq

        # Initialize ZMQ context and socket.
        # All messages are received in the same socket which means
        # that this test fails if the publishing order changes.
        # Note that the publishing order is not defined in the documentation and
        # is subject to change.
        self.zmq_context = zmq.Context()
        socket = self.zmq_context.socket(zmq.SUB)
        socket.set(zmq.RCVTIMEO, 60000)
        socket.connect(ADDRESS)

        # Subscribe to all available topics.
        self.hashblock = ZMQSubscriber(socket, b"hashblock")
        self.hashtx = ZMQSubscriber(socket, b"hashtx")
        self.rawblock = ZMQSubscriber(socket, b"rawblock")
        self.rawtx = ZMQSubscriber(socket, b"rawtx")

        self.extra_args = [
            ["-zmqpub%s=%s" % (sub.topic.decode(), ADDRESS) for sub in [self.hashblock, self.hashtx, self.rawblock, self.rawtx]],
            [],
        ]
        self.add_nodes(self.num_nodes, self.extra_args)
        self.start_nodes()

    def run_test(self):
        try:
            self._zmq_test()
        finally:
            # Destroy the ZMQ context.
            self.log.debug("Destroying ZMQ context")
            self.zmq_context.destroy(linger=None)

    def _zmq_test(self):
        num_blocks = 5
        self.log.info("Generate %(n)d blocks (and %(n)d coinbase txes)" % {"n": num_blocks})
        genhashes = self.nodes[0].generate(num_blocks)
        self.sync_all()

        for x in range(num_blocks):
            # Should receive the coinbase txid.
            txid = self.hashtx.receive()

            # Should receive the coinbase raw transaction.
            hex = self.rawtx.receive()
            assert_equal(hash256(hex), txid)

            # Should receive the generated block hash.
            hash = self.hashblock.receive().hex()
            assert_equal(genhashes[x], hash)
            # The block should only have the coinbase txid.
            assert_equal([txid.hex()], self.nodes[1].getblock(hash)["tx"])

            # Should receive the generated raw block.
            block = self.rawblock.receive()
            assert_equal(genhashes[x], bytes_to_hex_str(hash256(block[:80])))

        self.log.info("Wait for tx from second node")
        payment_txid = self.nodes[1].sendtoaddress(self.nodes[0].getnewaddress(), 1.0)
        self.sync_all()

        # Should receive the broadcasted txid.
        txid = self.hashtx.receive()
        assert_equal(payment_txid, txid.hex())

        # Should receive the broadcasted raw transaction.
        hex = self.rawtx.receive()
        assert_equal(payment_txid, hash256(hex).hex())

        self.log.info("Test the getzmqnotifications RPC")
        assert_equal(self.nodes[0].getzmqnotifications(), [
            {"type": "pubhashblock", "address": ADDRESS},
            {"type": "pubhashtx", "address": ADDRESS},
            {"type": "pubrawblock", "address": ADDRESS},
            {"type": "pubrawtx", "address": ADDRESS},
        ])

        assert_equal(self.nodes[1].getzmqnotifications(), [])

if __name__ == '__main__':
    ZMQTest().main()
