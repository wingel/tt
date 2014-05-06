# tt - a tiny terminal program for Linux

This is a very simple terminal program for Linux.  Actually it doesn't
really do anything having to do with terminal emulation, it leaves
that to whatever terminal tt is run from (in my case xterm).  But what
it does do is to talk to serial ports and contains some very simple
scripting to simplify configuration of serial ports.  I do a lot of
embedded programming, so a lightweight terminal program with logging
is a very good thing to have.  I've never really liked minicom or its
terminal emulation, in my opinion xterm does a much better job of it.

Start the program with "tt" and use "?" to see a list of available
commands.  For example, some commands to open, configure and then
start communicating with a serial port could look like this:

    set port /dev/ttyUSB0
    set speed 115200
    set flow none
    set modem off
    connect

The command set is somewhat inspired by ckermit.  It's also possible
to put commands in a file.  If tt is started with an argument it will
look for the file $HOME/.tt/ARGUMENT and execute any commands in that
file.  So I have put the above commands in $HOME/tt/usb, so that I can
start talking to a usb dongle by just typing "tt usb".

Another thing to note is that tt supports logging to a file.  So my
startup scripts actually also contain this line just before the
connect line:

    log append /home/wingel/tmp/usb.log

which means that everything I do is logged to a file so that I can
read the log file into emacs and look at it later.

Confession: In a way I'm a bit ashamed looking at code I wrote more
than a dozen years ago, this is not the way I would write things
today, but at the same time, this is a tool that I have been using a
lot so it's definitely useful.  So, well here it is.

