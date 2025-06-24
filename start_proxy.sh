#!/bin/bash
# script to start UDPProxy from cron
# assumes that keys.tdb is in $HOME/proxy
# assumes that UDPProxy build is in $HOME/UDPProxy

cd $HOME/proxy

(
    date
    pidof -q udpproxy || {
        nohup $HOME/UDPProxy/udpproxy >> proxy.log 2>&1 &
    }
) >> cron.log
