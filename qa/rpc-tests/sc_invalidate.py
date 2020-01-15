#!/usr/bin/env python2
# Copyright (c) 2014 The Bitcoin Core developers
# Copyright (c) 2018 The Zencash developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
from test_framework.test_framework import BitcoinTestFramework
from test_framework.authproxy import JSONRPCException
from test_framework.util import assert_equal, initialize_chain_clean, \
    start_nodes, stop_nodes, sync_blocks, sync_mempools, connect_nodes_bi, p2p_port
import os
from decimal import Decimal
import time

NUMB_OF_NODES = 3
DEBUG_MODE = 0
SC_COINS_MAT=2

class ScInvalidateTest(BitcoinTestFramework):
    alert_filename = None

    def setup_chain(self, split=False):
        print("Initializing test directory " + self.options.tmpdir)
        initialize_chain_clean(self.options.tmpdir, NUMB_OF_NODES)
        self.alert_filename = os.path.join(self.options.tmpdir, "alert.txt")
        with open(self.alert_filename, 'w'):
            pass  # Just open then close to create zero-length file

    def setup_network(self, split=False):
        self.nodes = []

        self.nodes = start_nodes(NUMB_OF_NODES, self.options.tmpdir,
                                 extra_args=[["-sccoinsmaturity=%d" % SC_COINS_MAT, '-logtimemicros=1', '-debug=sc',
                                              '-debug=py', '-debug=mempool', '-debug=net',
                                              '-debug=bench']] * NUMB_OF_NODES)

        if not split:
            # 1 and 2 are joint only if split==false
            connect_nodes_bi(self.nodes, 1, 2)
            sync_blocks(self.nodes[1:NUMB_OF_NODES])
            sync_mempools(self.nodes[1:NUMB_OF_NODES])

        connect_nodes_bi(self.nodes, 0, 1)
        self.is_network_split = split
        self.sync_all()

    def disconnect_nodes(self, from_connection, node_num):
        ip_port = "127.0.0.1:" + str(p2p_port(node_num))
        from_connection.disconnectnode(ip_port)
        # poll until version handshake complete to avoid race conditions
        # with transaction relaying
        while any(peer['version'] == 0 for peer in from_connection.getpeerinfo()):
            time.sleep(0.1)

    def split_network(self):
        # Split the network of three nodes into nodes 0-1 and 2.
        assert not self.is_network_split
        self.disconnect_nodes(self.nodes[1], 2)
        self.disconnect_nodes(self.nodes[2], 1)
        self.is_network_split = True

    def join_network(self):
        # Join the (previously split) network pieces together: 0-1-2
        assert self.is_network_split
        connect_nodes_bi(self.nodes, 1, 2)
        connect_nodes_bi(self.nodes, 2, 1)
        # self.sync_all()
        time.sleep(2)
        self.is_network_split = False

    def dump_ordered_tips(self, tip_list):
        if DEBUG_MODE == 0:
            return
        sorted_x = sorted(tip_list, key=lambda k: k['status'])
        c = 0
        for y in sorted_x:
            if (c == 0):
                print (y)
            else:
                print (" ", y)
            c = 1

    def dump_sc_info_record(self, info, i):
        if DEBUG_MODE == 0:
            return
        print ("  Node %d - balance: %f" % (i, info["balance"]))
        print ("    created in block: %s (%d)" % (info["created in block"], info["created at block height"]))
        print ("    created in tx:    %s" % info["creating tx hash"])
        print ("    immature amounts:  ", info["immature amounts"])

    def dump_sc_info(self, scId=""):
        if DEBUG_MODE == 0:
            return
        if scId != "":
            print ("scid: " + scId)
            print ("-------------------------------------------------------------------------------------")
            for i in range(0, NUMB_OF_NODES):
                try:
                    self.dump_sc_info_record(self.nodes[i].getscinfo(scId), i)
                except JSONRPCException, e:
                    print "  Node %d: ### [no such scid: %s]" % (i, scId)
        else:
            for i in range(0, NUMB_OF_NODES):
                x = self.nodes[i].getscinfo()
                for info in x:
                    self.dump_sc_info_record(info, i)

    def mark_logs(self, msg):
        if DEBUG_MODE == 0:
            return
        print (msg)
        self.nodes[0].dbg_log(msg)
        self.nodes[1].dbg_log(msg)
        self.nodes[2].dbg_log(msg)

    def run_test(self):
        ''' This test creates a Sidechain and forwards funds to it and then verifies
          after a fork that reverts the Sidechain creation, the forward transfer transactions to it
          are not in mempool
        '''
        # network topology: (0)--(1)--(2)

        blocks = [self.nodes[0].getblockhash(0)]

        fwt_amount_1 = Decimal("1.0")

        # node 1 earns some coins, they would be available after 100 blocks
        self.mark_logs("Node 1 generates 1 block")

        blocks.extend(self.nodes[1].generate(1))
        self.sync_all()

        # node 2 earns some coins, they would be available after 100 blocks
        self.mark_logs("Node 2 generates 1 block")

        blocks.extend(self.nodes[2].generate(1))
        self.sync_all()

        self.mark_logs("Node 0 generates 220 block")

        blocks.extend(self.nodes[0].generate(220))
        self.sync_all()

        txes = []

        # ---------------------------------------------------------------------------------------
        # Node 1 sends 10 coins to node 2 to have UTXO
        self.mark_logs("\n...Node 1 sends 10 coins to node 2 to have a UTXO")

        address_ds=self.nodes[2].getnewaddress()
        txid=self.nodes[1].sendtoaddress(address_ds,10.0)

        self.sync_all()

        blocks.extend(self.nodes[0].generate(2))

        self.sync_all()

        # Node 2 create rawtransaction that creates a SC using last UTXO
        self.mark_logs("\nNode 2 create rawtransaction that creates a SC using last UTXO...")

        decodedTx=self.nodes[2].decoderawtransaction(self.nodes[2].gettransaction(txid)['hex'])
        vout = {}
        for outpoint in decodedTx['vout']:
            if outpoint['value'] == Decimal('10.0'):
                vout = outpoint
                break;


        sc_address="0000000000000000000000000000000000000000000000000000000000000abc"
        scid="0000000000000000000000000000000000000000000000000000000000000022"
        sc_epoch=123
        sc_amount=Decimal('10.00000000')
        sc=[{"scid": scid,"epoch_length": sc_epoch}]
        inputs = [{'txid': txid, 'vout': vout['n']}]
        sc_ft=[{"address": sc_address, "amount":sc_amount, "scid": scid}]

        rawtx=self.nodes[2].createrawtransaction(inputs,{},sc,sc_ft)
        sigRawtx = self.nodes[2].signrawtransaction(rawtx)

        #Node 2 create rawtransaction with same UTXO
        self.mark_logs("\nNode 2 create rawtransaction with same UTXO...")

        outputs = {self.nodes[0].getnewaddress() : sc_amount}
        rawtx2=self.nodes[2].createrawtransaction(inputs,outputs)
        sigRawtx2 = self.nodes[2].signrawtransaction(rawtx2)


        # Split the network: (0)--(1) / (2)
        self.mark_logs("\nSplit network")
        self.split_network()
        self.mark_logs("The network is split: 0-1 .. 2")


        # Node 0 send the SC creation transaction and generate 1 block to create it
        self.mark_logs("\nNode 0 send the SC creation transaction and generate 1 block to create it")
        finalTx = self.nodes[0].sendrawtransaction(sigRawtx['hex'])
        txes.append(finalTx)
        self.sync_all()

        blocks.extend(self.nodes[0].generate(1))
        self.sync_all()

        # Node 1 creates a FT of 1.0 coin and Node 0 generates 1 block
        self.mark_logs("\nNode 1 sends " + str(fwt_amount_1) + " coins to SC")

        ftTx=self.nodes[1].sc_send("abcd", fwt_amount_1, scid)
        txes.append(ftTx)
        self.sync_all()

        self.mark_logs("\n...Node0 generating 1 block")
        blocks.extend(self.nodes[0].generate(1))
        self.sync_all()

        self.mark_logs ("\nChecking SC info on Node 2 that should not have any SC...")
        scinfoNode0 = self.nodes[0].getscinfo(scid)
        scinfoNode1 = self.nodes[1].getscinfo(scid)
        self.mark_logs("Node 0: " + str(scinfoNode0))
        self.mark_logs("Node 1: " + str(scinfoNode1))
        try:
            self.nodes[2].getscinfo(scid)
        except JSONRPCException, e:
            errorString = e.error['message']
            self.mark_logs (errorString)
        assert_equal("scid not yet created" in errorString, True)

        # Node 2 generate 4 blocks including the rawtx2 and now it has the longest fork
        self.mark_logs ("\nNode 2 generate 4 blocks including the rawtx2 and now it has the longest fork...")
        finalTx2 = self.nodes[2].sendrawtransaction(sigRawtx2['hex'])
        self.sync_all()

        self.nodes[2].generate(4)
        self.sync_all()

        self.mark_logs("\nJoining network")
        self.join_network()
        self.mark_logs("\nNetwork joined")

        # Checking the network chain tips
        self.mark_logs ("\nChecking network chain tips, Node 2's fork became active...")
        for i in range(0, NUMB_OF_NODES):
            chaintips = self.nodes[i].getchaintips()
            self.dump_ordered_tips(chaintips)

        # Checking SC info on network
        self.mark_logs ("\nChecking SC info on network, noone see the SC...")
        for i in range (0,NUMB_OF_NODES):
            try:
                self.nodes[i].getscinfo(scid)
            except JSONRPCException, e:
                errorString = e.error['message']
                self.mark_logs (errorString)
                assert_equal("scid not yet created" in errorString, True)

        # The FT transaction should not be in mempool
        self.mark_logs ("\nThe FT transaction should not be in mempool...")
        for i in range(0, NUMB_OF_NODES):
            txmem = self.nodes[i].getrawmempool()
            assert_equal(len(txmem), 0)



if __name__ == '__main__':
    ScInvalidateTest().main()
