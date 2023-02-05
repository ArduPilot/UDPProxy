# UDP Proxy for MAVLink

This is a UDP Proxy for MAVLink to facilitate remote support of
ArduPilot users

For more information on using the support proxy see https://support.ardupilot.org

## Features

 - both support engineer and user can be on private networks
 - supports manu users running in parallel
 - uses MAVLink2 signed connections from the support engineer
 - uses normal UDP forwarding in users GCS

## Setup

udpproxy should be run on a machine with a public IP address. You
should initialise the database 'keys.tdb' using the keydb.py script.

For example:

 - keydb.py add 10001 10002 'Support1' MySecurePassPhrase

that will add a single support engineer 'Support1' where the user will
connect to port 10001 and the support engineer will connect to port
10002.

Once the database is setup you should start udpproxy and it will
listen on all ports.

## keydb.py usage

The following keydb.py commands are available:

 - keydb.py initialise
 - keydb.py list
 - keydb.py add PORT1 PORT2 Name PassPhrase
 - keydb.py remove PORT2
 - keydb.py setname PORT2 Name
 - keydb.py setpass PORT2 NewPassPhrase
 - keydb.py setport1 PORT2 PORT1

When new users are added the udpproxy process starts listening on the
new port automatically.

## License

UDPProxy is licensed under the GNU General Public License version 3 or
later
