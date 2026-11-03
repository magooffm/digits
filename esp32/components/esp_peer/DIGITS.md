# esp_peer 1.5.7, patched for Digits

Espressif's `espressif/esp_peer` 1.5.7 (ESP32-S3 library only), used through
`override_path` in `main/idf_component.yml`. Changes, marked "Digits patch":

- `src/dtls_common.h`, `dtls_srtp_handshake_server`: the peer task calls the
  server handshake every ~56 ms while reads come back empty (the ClientHello
  reassembly BIO used below mbedTLS 3.6.6 gives up after 8 empty reads). The
  original reset the session on every call, so the server lost its state
  after sending its flight. Another ESP32 as DTLS client needs longer than
  one call for its ECDHE and ECDSA work, and the server then rejected its
  flight with `-0x7700` (unexpected message): two ESP32 phones never
  connected. Pion and browsers answer within the first call. Now the session
  is kept while a handshake is past ClientHello (`server_busy` in
  `dtls_srtp.h`, cleared in `dtls_srtp_reset_session`).
- The "Start to do server handshake" line is debug level, and the client no
  longer logs "CLient handshake fail -0x6900" while it merely waits.

Updating esp_peer: take the new release, reapply these changes if it still
resets on every call, and keep only `libs/esp32s3`.
