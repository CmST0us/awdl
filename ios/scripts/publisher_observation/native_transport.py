"""Use an existing paired Apple tunnel for one bounded RSD connection."""

def configure_native(native):
    """Use one RSD attempt and acquire an assertion without initiating pairing."""
    native._RSD_CONNECT_ATTEMPTS = 1
    native._SOCKET_TABLE_TIMEOUT = 2
    native._RemotePairingSession._REPLY_TIMEOUT = .5

    class ExistingSession(native._RemotePairingSession):
        def open_tunnel(self):
            xpc = self._xpc
            reply = self._request(
                self._device_conn, b'RemotePairing.CreateAssertionCommand',
                lambda body: xpc.dictionary_set_int64(body, b'flags', 0))
            try:
                response = xpc.dictionary_get_value(reply, b'response')
                if not response:
                    raise RuntimeError('existing tunnel assertion unavailable')
                assertion = xpc.dictionary_get_value(response, b'assertionIdentifier')
                if assertion:
                    self._assertion_id = xpc.retain(assertion)
                info = xpc.dictionary_get_value(response, b'info')
                address = xpc.dictionary_get_string(info, b'tunnelIPAddress') if info else None
                if not address:
                    raise RuntimeError('existing tunnel address unavailable')
                self.tunnel_ip = address.decode()
                return self.tunnel_ip
            finally:
                xpc.release(reply)

    native._RemotePairingSession = ExistingSession
