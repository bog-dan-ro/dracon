# Configuration

Everything is in the configuration directory, which is `etc/dracon` next to the
server binary unless `--conf` says otherwise. The format is Boost
property_tree's INFO syntax: `key value` a line, `;` starts a comment, `{}`
opens a section, and `#include "file"` pulls another file in.

```
key value               ; a comment
section {
    nested value
}
```

## server.conf

| Key | Default | |
|---|---|---|
| `queued_connections` | 20000 | The listen backlog |
| `max_connections_per_ip` | 500 | Connections accepted from one address |
| `headers_timeout` | 5 | Seconds to wait for a complete request head |
| `keepalive_timeout` | 10 | Seconds a connection stays open between requests |
| `workload_balancing` | true | Serve the addresses with the fewest connections first. Costs about 5% |
| `http_port` | 8080 | |
| `server_status` | false | Enable the built in `/server_status` endpoint |

`use_epoll_edge_trigger` is documented in the shipped `server.conf` but is not
read by the code. Edge triggered epoll is always on.

### logging

```
logging {
    #include "server_logging.conf"
}
```

The section is handed to Boost.Log's `init_from_settings`, so it takes sinks,
formats and filters. See [Logging](logging.md).

### privileges

```
privileges {
    drop false
    user daemon
    group daemon
}
```

Only used when the server was started as root, which is the case when it has to
bind a port below 1024. It binds the ports first and then becomes that user.
`--user` and `--group` on the command line override this section.

The shipped systemd unit does not use any of it. It starts the server as the
`dracon` user with `AmbientCapabilities=CAP_NET_BIND_SERVICE`, so the process
is never root and there is nothing to drop.

### https

```
https {
    enabled true
    port 8443
    accept_timeout 5        ; seconds for SSL_accept
    shutdown_timeout 2      ; seconds for SSL_shutdown

    ssl {
        ctx_method TLS      ; TLS or DTLS
        cxt_settings {
            #include "server_ssl_ctx.conf"
        }
    }
}
```

`cxt_settings` is passed to OpenSSL's `SSL_CONF_cmd` as raw key and value
pairs, so the TLS configuration is entirely OpenSSL's and entirely yours.
Certificate paths in it are relative to the configuration directory.

The shipped `server.crt` and `server.key` are a self signed pair, there so that
the https port works out of the box. Replace them before anything real reaches
the server. `conf/generate_cert.sh` makes a new pair, and `certbot-dracon/` has
a Let's Encrypt hook.

## Plugin configuration

A plugin reads its own file out of the same directory, conventionally
`<plugin name>.conf`, in the same format. `init_plugin()` is given the
directory.

### static_files.conf

The bundled `static_content` plugin:

```
default_file "index.html"
allow_symlinks false

paths {
;    "/~" "/home"          ; /~user/x.html -> /home/user/public_html/x.html
    "/" "/var/www"
}

custom_headers {
;    "Cross-Origin-Opener-Policy" "same-origin"
}
```

`paths` maps a URL prefix to a directory. The `"/~"` entry turns on
`/~user/` URLs, which resolve inside that user's `public_html`. Files are
served with `mmap` through an `lru_cache`, and the plugin sits at plugin order
`UINT32_MAX`, so it is always the last one asked.

Every filesystem failure is answered with a bare 404. Telling "no such file"
apart from "outside the document root" would turn the plugin into a way to
probe the filesystem.

## /server_status

`server_status true` in `server.conf` registers a pseudo plugin at order
`UINT32_MAX / 2` which answers `GET /server_status` with

```
Active sessions: 1
Sessions peak: 4
Uptime: 0 days, 2 hours, 13 minutes and 7 seconds
Served sessions: 918
```

It reads lock free counters and nothing else, so it can be polled as fast as
you like, and the body is only those counters: no paths, no versions, no
addresses. There is no authentication on it, so either keep it off or keep it
off the public interface.
