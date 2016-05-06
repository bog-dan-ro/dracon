# certbot-dracon

A [Certbot](https://certbot.eff.org/) plugin that obtains and installs certificates for the
[Dracon](https://github.com/bog-dan-ro/dracon) HTTP server.

It combines an authenticator (HTTP-01, by dropping challenge files into the directory Dracon's
`staticContent` plugin serves at `/`) and an installer (copies the issued certificate to
`/etc/dracon/server.crt`/`server.key`, turns on `https` in `server.conf` on first use, and
restarts `dracon.service`) in a single plugin, so it can be used as both:

```sh
certbot run --configurator dracon -d example.com
# equivalent to:
certbot run --authenticator dracon --installer dracon -d example.com
```

Requires `dracon-mod-staticcontent` (for the authenticator's challenge directory) and
`dracon-server` (for the installer).
