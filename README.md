    i2c, an utility for sequences of I2C operations

The utility was created to support semi-automated testing of devices on I2C
bus from a master node.

It takes a text input (either from a file or from stdin), interprets it and
issues a series of write(2) on a file handle corresponding the specified bus
interface.

Usage:

i2c [-h] [-f] [-t <i2c-timeout>] [-d <i2c-bus-name-or-full-path>] [-s <section-search>] <cmd-file>

`-h` Show usage information

`-f` Force use of the slave addresses. If given, uses `I2C_FORCE_SLAVE` ioctl
to set the slave address.

`-d <path>` Use this I2C bus, must be the device node file (e.g. `/dev/i2c-0`).

`-d <section-search>` Only send the defined section, nothing else. A
sub-string search is performed.

`<cmd-file>` The file (or `-` for stdin) with a sequence of commands in the format below:

    *( command [ commentary ] )
    [ "end" LF ]
    *section

    command := ( bus | delay | write | read | read-write ) LF
    bus := "bus " i2c-device-pathname
    delay := "delay " number ( | "us" | "ms" | "s" ) ; defaults to us
    write := "W " addr +byte
    read := "R " addr +byte ; currently not implemented
    read-write := addr-cmd +byte
    addr-cmd := addr << 1 | cmdbit ; cmdbit is 1 for read, 0 for write
    addr := byte
    byte := hex hex | "0x" hex hex | "0b" +("0" | "1") | "0o" oct oct oct | "0d" dec dec dec
    section := section-name LF *command "end" LF
    section-name := ": " arbitrary text...

The commands before the first section will be executed if no section-search
given.

Examples:

    echo "W 70 30 45" | i2c -d /dev/i2c-2 -

