"""Certbot authenticator + installer for the Dracon HTTP server.

The authenticator answers HTTP-01 challenges by dropping files into the
directory Dracon's staticContent plugin already serves at "/" (read from
static_files.conf, so no extra Dracon configuration is needed). The installer
copies an issued certificate into /etc/dracon, turns "https" on in
server.conf the first time it runs, and restarts dracon.service.

Usage: certbot run --configurator dracon -d example.com
"""
import grp
import logging
import os
import re
import shutil
import subprocess
from typing import Any, Callable, Iterable, Optional, Union

from acme import challenges

from certbot import errors
from certbot.achallenges import AnnotatedChallenge
from certbot.plugins import common

logger = logging.getLogger(__name__)

DEFAULT_STATIC_FILES_CONF = "/etc/dracon/static_files.conf"
DEFAULT_WEBROOT = "/var/www"
DEFAULT_CERT_PATH = "/etc/dracon/server.crt"
DEFAULT_KEY_PATH = "/etc/dracon/server.key"
DEFAULT_SERVER_CONF = "/etc/dracon/server.conf"
DEFAULT_KEY_GROUP = "dracon"


def _default_webroot() -> str:
    """Best-effort read of the "/" -> path mapping from static_files.conf.

    static_files.conf is Boost property_tree INFO, a format with no Python
    parser available; this only needs the one line every stock config has,
    a quoted "/" key followed by a quoted directory in the "paths" section.
    """
    try:
        with open(DEFAULT_STATIC_FILES_CONF, encoding="utf-8") as conf_file:
            text = conf_file.read()
    except OSError:
        return DEFAULT_WEBROOT
    match = re.search(r'"/"\s+"([^"]+)"', text)
    return match.group(1) if match else DEFAULT_WEBROOT


class Configurator(common.Configurator):
    """Authenticator and installer for Dracon."""

    description = "Obtain and install certificates for the Dracon HTTP server"

    def __init__(self, *args: Any, **kwargs: Any) -> None:
        super().__init__(*args, **kwargs)
        self._webroot: Optional[str] = None
        self._performed: dict[AnnotatedChallenge, str] = {}

    @classmethod
    def add_parser_arguments(cls, add: Callable[..., None]) -> None:
        add("webroot-path", default=None,
            help="Directory Dracon serves at \"/\" (default: read from {0}, "
                 "falling back to {1})".format(DEFAULT_STATIC_FILES_CONF, DEFAULT_WEBROOT))
        add("cert-path", default=DEFAULT_CERT_PATH,
            help="Where to install the certificate")
        add("key-path", default=DEFAULT_KEY_PATH,
            help="Where to install the private key")
        add("key-group", default=DEFAULT_KEY_GROUP,
            help="Group given read access to the installed private key")
        add("server-conf", default=DEFAULT_SERVER_CONF,
            help="Path to Dracon's server.conf")

    def prepare(self) -> None:
        self._webroot = self.conf("webroot-path") or _default_webroot()
        if not os.path.isdir(self._webroot):
            raise errors.PluginError(
                "{0} does not exist; is dracon-mod-staticcontent installed and "
                "configured to serve \"/\" from it?".format(self._webroot))
        if shutil.which("systemctl") is None:
            raise errors.NoInstallationError("systemctl was not found")
        if not os.path.isdir("/etc/dracon"):
            raise errors.NoInstallationError(
                "/etc/dracon does not exist; is dracon-server installed?")

    def more_info(self) -> str:
        return (
            "Places http-01 challenge files under {0}/.well-known/acme-challenge/, "
            "installs certificates to {1} and {2}, and restarts dracon.service."
        ).format(self._webroot, self.conf("cert-path"), self.conf("key-path"))

    def auth_hint(self, failed_achalls: list) -> str:
        """Explain the failure the CA actually reported.

        An http-01 validation can fail at three very different places and only
        the last one is Dracon's business: the name may not resolve, the CA may
        not reach port 80, or it may reach Dracon and get the wrong bytes back.
        Certbot hands us the ACME error per challenge (auth_handler only
        collects challenges that have one), so group by error code and answer
        the question that was actually asked instead of always blaming the
        webroot - a "check your static_files.conf" hint under an NXDOMAIN sends
        people to debug a server that was never contacted.
        """
        by_code: dict[Optional[str], list[str]] = {}
        for achall in failed_achalls:
            error = getattr(achall, "error", None)
            by_code.setdefault(getattr(error, "code", None), []).append(achall.domain)

        hints = []
        for code, domains in by_code.items():
            names = ", ".join(sorted(set(domains)))
            if code in ("dns", "unknownHost", "dnssec"):
                hints.append(
                    "{0} does not resolve for the Certificate Authority, so nothing on "
                    "this machine was ever contacted - this is not a Dracon problem. "
                    "Check the spelling of the domain, then point an A (and/or AAAA) "
                    "record at this server's public address and wait for it to "
                    "propagate.".format(names))
            elif code in ("connection", "tls"):
                hints.append(
                    "{0} resolves, but the Certificate Authority could not reach it on "
                    "port 80. Validation always starts on port 80, even when you only "
                    "want HTTPS. Check that dracon.service is running (systemctl status "
                    "dracon), that server.conf's http_port is 80, and that a firewall or "
                    "NAT in front of this host forwards port 80 to it.".format(names))
            elif code in ("unauthorized", "incorrectResponse"):
                hints.append(
                    "The Certificate Authority reached Dracon for {0} but did not get "
                    "the challenge files Certbot wrote under "
                    "{1}/.well-known/acme-challenge/. Make sure dracon-mod-staticcontent "
                    "is installed, that static_files.conf's \"/\" mapping points at {1}, "
                    "and that the dracon user can read it.".format(names, self._webroot))
            elif code == "caa":
                hints.append(
                    "CAA records for {0} forbid this Certificate Authority from issuing. "
                    "Fix them in DNS; Dracon is not involved.".format(names))
            elif code == "rateLimited":
                hints.append(
                    "You are being rate limited for {0}. Wait it out rather than "
                    "changing anything here, and use --dry-run while "
                    "experimenting.".format(names))
            else:
                hints.append(
                    "Validation of {0} failed before Dracon could serve anything useful. "
                    "The Detail line above is the Certificate Authority's own words; if "
                    "it mentions the challenge files, they were written under "
                    "{1}/.well-known/acme-challenge/.".format(names, self._webroot))
        return " ".join(hints)

    # ---- Authenticator (HTTP-01) ----

    def get_chall_pref(self, unused_identifier: str) -> Iterable[type]:
        return [challenges.HTTP01]

    def perform(self, achalls: list) -> list:
        responses = []
        for achall in achalls:
            response, validation = achall.response_and_validation()
            challenge_dir = os.path.join(self._webroot, ".well-known", "acme-challenge")
            os.makedirs(challenge_dir, exist_ok=True)
            os.chmod(os.path.join(self._webroot, ".well-known"), 0o755)
            os.chmod(challenge_dir, 0o755)
            path = os.path.join(challenge_dir, achall.chall.encode("token"))
            with open(path, "w", encoding="utf-8") as validation_file:
                validation_file.write(validation)
            os.chmod(path, 0o644)
            self._performed[achall] = path
            responses.append(response)
        return responses

    def cleanup(self, achalls: list) -> None:
        for achall in achalls:
            path = self._performed.pop(achall, None)
            if path is not None:
                try:
                    os.remove(path)
                except OSError as error:
                    logger.debug("Couldn't remove %s: %s", path, error)

    # ---- Installer ----

    def get_all_names(self) -> Iterable[str]:
        # Dracon has no virtual host concept: it serves one certificate for
        # whatever domains the admin passes on the command line.
        return []

    def _checkpoint(self, paths: set[str], notes: str) -> None:
        """Put paths under Certbot's rollback control, existing or not.

        add_to_checkpoint() backs a file up by copying it, so it only works on
        files that are already there. The package deliberately ships no
        certificate, so on a first run server.crt and server.key do not exist
        yet and the whole install step used to die with "Unable to add file
        /etc/dracon/server.crt to checkpoint". Files that do not exist are
        registered as creations instead, which is what makes a rollback delete
        them rather than restore them.
        """
        existing = {path for path in paths if os.path.exists(path)}
        created = paths - existing
        if existing:
            self.add_to_checkpoint(existing, notes)
        if created:
            try:
                # False: a permanent checkpoint, matching add_to_checkpoint above.
                self.reverter.register_file_creation(False, *sorted(created))
            except errors.ReverterError as error:
                raise errors.PluginError(str(error))

    def deploy_cert(self, domain: str, cert_path: str, key_path: str,
                    chain_path: str, fullchain_path: str) -> None:
        del cert_path, chain_path  # server_ssl_ctx.conf wants the fullchain
        cert_dest = self.conf("cert-path")
        key_dest = self.conf("key-path")
        self._checkpoint({cert_dest, key_dest},
                         "Dracon certificate for {0}".format(domain))
        shutil.copyfile(fullchain_path, cert_dest)
        shutil.copyfile(key_path, key_dest)
        os.chmod(cert_dest, 0o644)
        os.chmod(key_dest, 0o640)
        try:
            gid = grp.getgrnam(self.conf("key-group")).gr_gid
        except KeyError:
            logger.warning(
                "Group %s doesn't exist; leaving %s readable by root only",
                self.conf("key-group"), key_dest)
        else:
            try:
                os.chown(key_dest, 0, gid)
            except OSError as error:
                # The key is already 0640, so the only thing lost here is
                # Dracon's ability to read it; say so instead of unwinding a
                # deploy that otherwise succeeded.
                logger.warning(
                    "Couldn't give group %s read access to %s (%s); Dracon will "
                    "not be able to read its private key",
                    self.conf("key-group"), key_dest, error)

    def enhance(self, domain: str, enhancement: str,
                options: Optional[Union[list, str]] = None) -> None:
        del domain, options
        raise errors.PluginError("Unsupported enhancement: {0}".format(enhancement))

    def supported_enhancements(self) -> list:
        return []

    def save(self, title: Optional[str] = None, temporary: bool = False) -> None:
        if title and not temporary:
            self.finalize_checkpoint(title)

    def config_test(self) -> None:
        # Dracon has no offline "check the config" mode; restart() is about
        # to start the real thing anyway, which is the only real test.
        pass

    def restart(self) -> None:
        self._enable_https()
        try:
            subprocess.run(
                ["systemctl", "reload-or-restart", "dracon.service"],
                check=True, capture_output=True)
        except FileNotFoundError as error:
            raise errors.PluginError(str(error))
        except subprocess.CalledProcessError as error:
            stderr = error.stderr.decode(errors="replace") if error.stderr else str(error)
            raise errors.PluginError("dracon.service failed to (re)start: {0}".format(stderr))

    def _enable_https(self) -> None:
        """Flip the shipped "https { enabled false ... }" to true, once."""
        conf_path = self.conf("server-conf")
        try:
            with open(conf_path, encoding="utf-8") as conf_file:
                text = conf_file.read()
        except OSError as error:
            raise errors.PluginError("Can't read {0}: {1}".format(conf_path, error))

        https_pos = text.find("https {")
        if https_pos == -1:
            logger.warning("No \"https\" section found in %s; leaving it alone", conf_path)
            return
        head, tail = text[:https_pos], text[https_pos:]
        new_tail, count = re.subn(
            r"(?m)^(\s*)enabled\s+false\b.*$", r"\1enabled true", tail, count=1)
        if count == 0:
            return  # already enabled, or the admin changed the line's shape
        self._checkpoint({conf_path}, "Enable HTTPS in Dracon's server.conf")
        with open(conf_path, "w", encoding="utf-8") as conf_file:
            conf_file.write(head + new_tail)
