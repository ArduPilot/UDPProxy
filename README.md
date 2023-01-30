# UDP Proxy for MAVLink

This is a UDP Proxy for MAVLink to facilitate remote support of
ArduPilot users

## Features

 - both support engineer and user can be on private networks
 - supports manu users running in parallel
 - supports MAVLink2 signed connections from the support engineer
 - uses normal UDP forwarding in users GCS

## Setup

udpproxy should be run on a machine with a public IP address. You need
to specify a base UDP port number for users to connect to and a base
UDP port number for the support engineers to connnect to.

For example:

 - udpproxy 20000 21000 30

this will start 30 UDP proxy instances with user ports 20000 to 20029
and support ports 21000 to 21029.

A cron job should be setup to restart udpproxy if needed

## Setting signing keys

By default support engineers can connect without a signed
connection. As this poses a security risk it is highly recommended to
setup a signing key per support engineer.

Example:

 - set_key 21001 MySecretKey

this will setup the support engineer using port 21001 with the signing
key MySecretKey.
