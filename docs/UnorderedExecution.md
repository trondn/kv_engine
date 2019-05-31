# Unordered Execution Mode

Initially all connections to memcached is in an ordered execution
mode. That means that the server completes one command _before_ it
starts executing the next command. This mode has a number of pros and
a number of cons, but the biggest problem is that one slow operation
blocks the entire pipeline of commands.

To work around that problem the client may toggle the connection into
unordered execution mode by using [HELO](BinaryProtocol.md#0x1f-helo)
command. When enabled the client tells the server that it is free
to optimize the execution order of commands with
[reorder](BinaryProtocol.md#request-header-with-flexible-framing-extras")
specified. If the client send the following pipeline:

    cmd1
    cmd2 [reorder]
    cmd3 [reorder]
    cmd4
    
The server must execute `cmd1` _before_ it may start execution of
`cmd2`. The server may execute `cmd2` and `cmd3` in any order it
like (even in parallel), but it has to wait until both commands is
completed before it may start executing `cmd4`.

The server gives the client full freedom to do stupid things like:

    SET foo {somevalue} [reorder]
    SET foo {someothervalue} [reorder]
    APPEND foo {somethirdvalue} [reorder]
    GET foo [reorder]

NOTE: Unordered Execution Mode is mutually exclusive with DCP. You
can't enable unordered execution mode on a connection configured for
DCP, and you cannot start DCP on a connection set in unordered execution
mode.

NOTE: The client may request `reorder` on all commands and the server
will silently ignore the flag if the connection is set to allow
unordered execution (except DCP commands.. They will _fail_) and
execute the commands sequentially if its not implemented in the

The client may use the opaque field in the request to identify the
which request the response belongs to.

## Commands currently supported in the server

The following is a list of commands the client may use
with the reorder flag set and if something weird happens
it should be considered an ERROR in the server and a bug
report should be filed. The server do accept other commands
(see the next chapter), but clients should _NOT_ try to
use them at this time.

* Get (including quiet versions with and without key)
* Get Replica
* Get locked
* Get and touch
* Touch
* Unlock
* Incr / decr (including quiet versions)
* Delete (including quiet version)
* Add, Set, Replace, append, prepend (including quiet versions)

## Commands currently accepted by the server

The following is the list of opcodes the core honors the
reorder flag for. As stated above the client may use a
sequence of commands which would cause "unexpected"
results in the client (for instance one could enable
phosphor tracing in ioctl set, and it could collect
information from the "previous" commands as they was
blocked when it was enabled).

The reason some of the commands listed here is on this
list is that they _don't_ block internally in the server.
by keeping them in this list we don't have to drain the
execution pipe before starting to execute them.

* Get (including quiet versions with and without key)
* Get locked
* Get and touch
* Touch
* Unlock
* Sasl list mech
* Delete (including quiet version)
* Isasl Refresh
* SSL cert Refresh
* List buckets
* Get meta (including quiet version)
* Verbosity
* Audit put
* Incr / decr (including quiet versions)
* Ioctl get
* Ioctl set
* Config validate
* Config reload
* Audit config reload
* Version
* Get Error Map
* Auth provider
* RBAC refresh
* Evict key
* Get CTRL Token
* Get Replica
* GetClusterConfig
* SetClusterConfig
* Add, Set, Replace (including quiet versions)
* Append, Prepend (including quiet versions)
* Scrub
* GetCmdTimer
* Adjust timeofday

## TODOs

### Perf

Perform a perf run and get us back in the same ballpark

### Whitelist more commands
