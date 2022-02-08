# tio - a simple TTY terminal I/O tool

[![Build Status](https://travis-ci.org/tio/tio.svg?branch=master)](https://travis-ci.org/tio/tio)

## 1. Introduction

tio is a simple TTY terminal tool which features a straightforward command-line
interface to easily connect to TTY devices for basic I/O operations.

<p align="center">
<img src="images/tio-demo.gif">
</p>


## 2. Usage

The command-line interface is straightforward as reflected in the output from
'tio --help':
```
    Usage: tio [<options>] <tty-device>

    Options:
      -b, --baudrate <bps>        Baud rate (default: 115200)
      -d, --databits 5|6|7|8      Data bits (default: 8)
      -f, --flow hard|soft|none   Flow control (default: none)
      -s, --stopbits 1|2          Stop bits (default: 1)
      -p, --parity odd|even|none  Parity (default: none)
      -o, --output-delay <ms>     Output delay (default: 0)
      -n, --no-autoconnect        Disable automatic connect
      -e, --local-echo            Do local echo
      -t, --timestamp             Timestamp lines
      -l, --log <filename>        Log to file
      -m, --map <flags>           Map special characters
      -v, --version               Display version
      -h, --help                  Display help

    See the man page for list of supported mapping flags.

    In session, press ctrl-t q to quit.
```

The only option which requires a bit of elaboration is perhaps the
`--no-autoconnect` option.

By default tio automatically connects to the provided device if present.  If
the device is not present, it will wait for it to appear and then connect. If
the connection is lost (eg. device is unplugged), it will wait for the device
to reappear and then reconnect. However, if the `--no-autoconnect` option is
provided, tio will exit if the device is not present or an established
connection is lost.

Tio features full bash autocompletion support.

Tio also supports various key commands. Press ctrl-t ? to list the available
key commands.

See the tio man page for more details.


## 3. Installation

### 3.1 Installation using package manager
tio comes prepackaged for various GNU/Linux distributions. Please consult your package manager tool to find and install tio.

### 3.2 Installation using snap

Install latest stable version:
```
    $ snap install tio
```
Install bleeding edge:
```
    $ snap install tio --edge
```

### 3.3 Installation from source

The latest source releases can be found [here](https://github.com/tio/tio/releases).

Install steps:
```
     $ ./configure
     $ make
     $ make install
```
See INSTALL file for more installation details.


## 4. Contributing

Tio is open source. All contributions (bug fixes, doc, ideas, etc.) are
welcome. Visit the tio GitHub page to access latest source code, create pull
requests, add issues etc..

GitHub: https://github.com/tio/tio

Also, if you find this free open source software useful please consider making
a donation of your choice:

[![Donate](images/paypal.png)](https://www.paypal.me/lundmar)


## 5. Support

Submit bug reports via GitHub: https://github.com/tio/tio/issues


## 6. Website

Visit [tio.github.io](https://tio.github.io)


## 7. License

Tio is GPLv2+. See COPYING file for license details.


## 8. Authors

Created by Martin Lund \<martin.lund@keep-it-simple.com>

See the AUTHORS file for full list of contributors.
