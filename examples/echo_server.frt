\\ Simple TCP echo server on 127.0.0.1:7000

uv:tcp dup "0.0.0.0" 7000 uv:tcp-bind
dup 128 [ [ uv:write ] uv:read-start ] uv:listen
uv:run
