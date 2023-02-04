#!/bin/bash

# this is an example script for launching support with a given support
# ID. The AIRCRAFT name is used to give the directory that mavlink
# logs will be stored

SIGNING_KEY="MYPASSPHRASE"

[ $# -ge 2 ] || {
	echo "USAGE: mav_support.sh AIRCRAFT SUPPORT_ID"
	exit 1
}

AIRCRAFT="$1"
SUPPORT_ID="$2"
shift
shift


mavproxy.py --cmd="signing key $SIGNING_KEY" --master udpout:support.ardupilot.org:$SUPPORT_ID --aircraft "$AIRCRAFT" --console --map $*
