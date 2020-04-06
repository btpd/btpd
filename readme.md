# BTPD

BitTorrent Protocol Daemon

## Index

1. [Introduction](#introduction)
	* [What is btpd?](#what-is-btpd?)
	* [The programs](#the-programs)
	* [The btcli commands](#the-btcli-commands)
2. [Using btpd](#using-btpd)
	* [First](#first)
	* [Starting btpd](#starting-btpd)
	* [Examples](#examples)
	* [Troubleshooting](#troubleshooting)
3. [Building](#building)
	* [Requirements](#requirements)
	* [Standard build](#standard-build)
4. [Additional notes](#additional-notes)
	* [Upgrade from old versions](#upgrade-from-old-versions)
	* [Pre exit mode](#pre-exit-mode)
	* [Using both IPv6 and IPv4](#using-both-ipv6-and-ipv4)

## Introduction

### What is btpd?

`btpd` is a utility for sharing files over the BitTorrent network protocol.
It runs in daemon mode, thus needing no controlling terminal or gui.
Instead, the daemon is controlled by btcli, its command line utility,
or other programs capable of sending commands and queries on the control
socket.

### The programs

`btpd` consists of the following programs:
* `btpd`     - The bittorrent client.
* `btcli`    - Command line interface to btpd.
* `btinfo`   - Shows information from a torrent file.

All programs accept the `--help` option.

### The btcli commands

The `btcli` utility has several different modes of operation. One of the following commands must be specified when running `btcli`:
* `add`      - Add torrents to btpd.
* `del`      - Remove torrents from btpd.
* `kill`     - Shut down btpd.
* `list`     - List torrents.
* `rate`     - Set the global up and download rates in KB/s.
* `start`    - Activate torrents.
* `stat`     - Display stats for active torrents.
* `stop`     - Deactivate torrents.

## Using btpd

### First

To start sharing a torrent with btpd, the torrent needs to be added to
btpd. This is done with `btcli add`. When you add a torrent btpd automatically (if you didn't specify otherwise) starts to share the torrent
and download any missing data. The content directory you specify when
adding a torrent doesn't need to exist; it will be created by btpd.

You can see which torrents have been added to btpd with `btcli list`.
The list command also displays a number for each added torrent. This number
can be used to specify the target torrent for the btcli commands, so you
don't have to keep the torrent file once you've added it.

The up- and download progress can be followed by using the `btcli stat`
command. Both the list and stat commands use the following indicators to
display the state of a torrent:
```
+     the torrent is starting. This may take time if btpd needs to test
      the content of this torrent or one started before it.
-     the torrent is being stopped.
I     the torrent is inactive.
S     btpd is seeding the torrent.
L     btpd is leeching the torrent.
```

You can stop an active torrent with `btcli stop` and, of course,
start an inactive torrent by using `btcli start`.

The `btcli del` command should only be used when you're totally finished
with sharing a torrent. The command will remove the torrent and its
associated data from btpd. It's an escpecially bad idea to remove a not
fully downloaded torrent and then adding it again, since btpd has lost
information on the not fully downloaded pieces and will need to download
the data again.

To shut down btpd use `btcli kill`. Don't forget to read the help for each
of btcli's commands.

### Starting btpd

NOTE: You should only need one instance of btpd regardless of how many
torrents you want to share.

To start btpd with default settings you only need to run it. However,
there are many useful options you may want to use. To see a full list
run `btpd --help`. If you didn't specify otherwise,  btpd starts with
the same set of active torrents as it had the last time it was shut down.

btdp will store information and write its log in `$HOME/.btpd`. Therefore
it needs to be able to write there during its execution. You can specify
another directory via the `-d` option or the `$BTPD_HOME` variable.

I recommend specifiying the maximum number of uploads. Bittorrent employs a
tit for tat algorithm, so uploading at good rates allows for downloading.
Try to find a balance between uploads/outgoing bandwidth and the number of
active torrents.

### Examples

Start btpd with all options set to their default values.
```
# btpd
```

Start btpd and make it listen on port 12345, limit outgoing bandwidth to
200kB/s, limit the number of peers to 40 and not start any torrents that
were active the last time btpd was shut down.
```
# btpd -p 12345 --bw-out 200 --max-peers 40 --empty-start
```

Display a list btpd's torrents and their number, size, status, etc.
```
# btcli list
```

Same as above, but only for torrent 12 and my.little.torrent.
```
# btcli list 12 my.little.torrent
```

Same as above but only for active torrents.
```
# btcli list -a
```

Same as above, but print using a custom format
```
# btcli list -a -f "btcli list -f "%n\t%#\t%p%s\t%r\n"
```

Add foo.torrent, with content dir foo.torrent.d, and start it.
```
# btcli add -d foo.torrent.d foo.torrent
```

Same as above without starting it.
```
# btcli add --no-start -d foo.torrent.d foo.torrent 
```

Start bar.torrent and torrent number 7.
```
# btcli start bar.torrent 7
```

Stop torrent number 7.
```
# btcli stop 7
```

Stop all active torrents.
```
# btcli stop -a
```

Remove bar.torrent and it's associated information from btpd.
```
# btcli del bar.torrent
```

Display a summary of up/download stats for the active torrents.
```
# btcli stat
```

Display the summary once every five seconds.
```
# btcli stat -w 5
```

Same as above, but also display individual stats for each active torrent.
```
# btcli stat -w 5 -i
```

Set the global upload rate to 20KB/s and download rate to 1MB/s.
```
# btcli rate 20K 1M
```

Shut down btpd.
```
# btcli kill
```

### Troubleshooting

If btpd has shut down for some unknown reason, check the logfile for
possible clues.

## Building

### Requirements

You should have a *BSD, Linux or sufficiently similar system.

Make sure you have recent versions of the following software:
* OpenSSL   - Get at http://www.openssl.org/

You also need a c99 compiler. A non antique GCC should do.

### Standard build

```
# ./configure
# make
# make install
```

See `./configure --help` for available build options if the above fails.

## Additional notes

### Upgrade from old version

The layout of the torrents directory in the btpd directory has changed
since btpd 0.11. Please remove the torrents directory before running
later versions.

### Pre exit mode

If btpd needs to send stop messages to trackers before shutting down,
it will enter the pre exit mode. A btpd process in this mode can safely
be ignored and will not interfere with any new btpd started in the same
directory.

### Using both IPv6 and IPv4

Unfortunately enabling both IPv6 and IPv4 in btpd is less useful than it
should be. The problem is that some sites have trackers for both versions
and it's likely that the IPv6 one, which probably has less peers, will be
used in favour of the IPv4 one.

In order to fix this problem, the IP version options should be changed to
be per torrent, in some future version of btpd.
