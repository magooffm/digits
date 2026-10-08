Ich habe CODEX.md vollständig gelesen und Server, Pi-Client sowie Pico-Firmware untersucht. Eine ESP32-Anbindung erscheint ohne Änderung am Server grundsätzlich möglich. Offen bleibt die tatsächliche WebRTC-Interoperabilität mit Espressifs Stack.
Es wurden keine Dateien verändert. Die Analyse bezieht sich auf den lokalen Stand a4edaaa; die Übereinstimmung mit deinem laufenden signald v1.99.0 ist noch nicht überprüft.
1. Relevante Komponenten
Komponente	Aufgabe und Referenz
server/	Gerätezuordnung, Authentifizierung, Rufvermittlung und Weiterleitung von SDP/ICE. Handler.handleWS, Relay.HandleMessage in [handler_ws.go](/Users/marcomhopp/Documents/GitHub/digits/server/internal/web/handler_ws.go) und [relay.go](/Users/marcomhopp/Documents/GitHub/digits/server/internal/signaling/relay.go).
pi/digitsd/	Telefonsteuerung, Signaling, WebRTC und Audio. daemonCallbacks.InitiateCall, AnswerCall, HangupCall in [webrtc.go](/Users/marcomhopp/Documents/GitHub/digits/pi/digitsd/cmd/digitsd/webrtc.go).
firmware/	Pico-Firmware für Hook, Tasten, Klingel und lokale Zustände. hook_poll, phone_fsm_update in [hook.c](/Users/marcomhopp/Documents/GitHub/digits/firmware/src/hook.c) und [phone_fsm.c](/Users/marcomhopp/Documents/GitHub/digits/firmware/src/phone_fsm.c).
pi/image/	Linux-Systemintegration einschließlich Audiokonfiguration, Netzwerk und Diensten. Für ESP32 überwiegend zu ersetzen.


Ein esp32/-Verzeichnis existiert bisher nicht.
2. Identität, Provisionierung und Authentifizierung
Die Geräteidentität ist beim Pi eine dauerhaft gespeicherte, zufällig erzeugte UUID v4. hardware_id ist also keine ausgelesene Hardware-Seriennummer. Referenz: LoadOrCreateDeviceID in [deviceid.go](/Users/marcomhopp/Documents/GitHub/digits/pi/digitsd/internal/config/deviceid.go).
WLAN-Einrichtung und Digits-Pairing sind getrennte Schritte:
1. Der Pi richtet WLAN über einen lokalen Setup-Webserver ein: runSetupMode in [setup.go](/Users/marcomhopp/Documents/GitHub/digits/pi/digitsd/cmd/digitsd/setup.go).
2. Das Gerät öffnet /ws und sendet register mit number, hardware_id und gegebenenfalls device_token. Ohne zugewiesene Nummer verwendet der Pi zunächst "unpaired".
3. Für ein ungepaartes Gerät liefert der Server pairing_code. Der lokale Code erzeugt sechs Ziffern mit zehn Minuten Gültigkeit; erneute Registrierung ersetzt den vorherigen Code.
4. Ein angemeldeter Household-Administrator ordnet den Code über die Weboberfläche einer neuen oder bestehenden Leitung zu.
5. Der Server sendet paired mit Rufnummer und Gerätetoken. Der Client speichert beides und verbindet sich anschließend erneut.
Referenzen: Client.Connect in [client.go](/Users/marcomhopp/Documents/GitHub/digits/pi/digitsd/internal/signal/client.go), Handler.handleWS, Store.GenerateCode/ClaimDevice/ClaimDeviceToLine in [pairing.go](/Users/marcomhopp/Documents/GitHub/digits/server/internal/pairing/pairing.go), handlePhonesPairPost in [handler_phones.go](/Users/marcomhopp/Documents/GitHub/digits/server/internal/web/handler_phones.go).
Das Telefon benötigt keinen Browser-Login und keinen Session-Cookie. Die Geräteauthentifizierung erfolgt im ersten WebSocket-JSON mit dem Token. Der Server speichert dessen Hash und prüft ihn zusammen mit der Geräteidentität. Die serverseitig gebundene Leitung ist maßgeblich; eine veraltete Rufnummer kann mit line_renumber korrigiert werden.
Referenzen: Store.AuthStatus/BoundLineNumber in [store.go](/Users/marcomhopp/Documents/GitHub/digits/server/internal/device/store.go), Handler.handleWS.
Auf dem ESP32 müssen mindestens Geräteidentität, Serveradresse, Rufnummer, Token und WLAN-Zugangsdaten persistent gespeichert werden. Die Pi-Konfiguration definiert die Digits-Felder in Config: [config.go](/Users/marcomhopp/Documents/GitHub/digits/pi/digitsd/internal/config/config.go).
3. Signaling-Transport und Nachrichten
Der Transport ist WebSocket mit JSON-Textnachrichten über /ws. Die Registrierung muss innerhalb von zehn Sekunden eintreffen. Der Server sendet alle 30 Sekunden WebSocket-Pings und erwartet Pongs innerhalb seines 45-Sekunden-Fensters. Referenz: Handler.handleWS.
Die wesentlichen Nachrichten sind:
Nachricht	Bedeutung / wichtige Felder
register	Anmeldung: number, hardware_id, device_token
pairing_code, paired	Pairing-Code beziehungsweise Token und zugewiesene Nummer
call	Ausgehender Ruf: to
ring	Eingehender Ruf: from
sdp	Bei normalem Ruf das SDP-Angebot: to, sdp
answer	Rufannahme einschließlich SDP-Antwort: to, sdp
ice	Einzelner ICE-Kandidat als String: to, candidate
hangup	Rufende; optional reason
busy, error	Besetzt oder Fehler
request-ice-servers, ice-servers	STUN-/TURN-Konfiguration
ice_restart	Neues SDP-Angebot zur ICE-Wiederherstellung
line_settings, line_renumber	Einstellungen beziehungsweise korrigierte Rufnummer


Die vollständigen Wire-Typen stehen in Message und den Type…-Konstanten in [protocol.go](/Users/marcomhopp/Documents/GitHub/digits/server/internal/signaling/protocol.go).
Online/offline wird aus registrierten WebSocket-Verbindungen abgeleitet; dafür gibt es im untersuchten Basisablauf keine separate Telefon-Nachricht. Referenzen: Handler.handleWS, Hub.Register/Unregister in [hub.go](/Users/marcomhopp/Documents/GitHub/digits/server/internal/signaling/hub.go).
4. WebRTC und Rufablauf
Der normale Ablauf ist:
1. Anrufer sendet call.
2. Server prüft Erreichbarkeit, Rufberechtigung und Besetztzustand und sendet ring.
3. Anrufer erzeugt eine PeerConnection samt Audiotrack und sendet das Angebot als sdp.
4. ICE-Kandidaten folgen als ice; der Pi stellt sicher, dass das Angebot zuerst versendet wird.
5. Empfänger speichert Angebot und Kandidaten während des Klingelns. Er kann die PeerConnection bereits vorbereiten, hält Antwort und lokale Kandidaten aber bis zur Annahme zurück.
6. Beim Abheben sendet der Empfänger answer mit SDP-Antwort, danach lokale ICE-Kandidaten.
7. Auflegen beendet Signaling und lokale Medienressourcen.
Referenzen: Relay.handleCall/handleAnswer/handleHangup, daemonCallbacks.InitiateCall/AnswerCall/prepareAnswer/sendPreparedAnswer, handleSignal in [dispatch.go](/Users/marcomhopp/Documents/GitHub/digits/pi/digitsd/cmd/digitsd/dispatch.go).
Der Server erzeugt keine PeerConnection und wandelt SDP nicht um. Er leitet SDP und ICE weiter, allerdings nur innerhalb eines bekannten Rufes beziehungsweise einer gültigen Konferenz. Er setzt from anhand der registrierten Verbindung selbst. Referenzen: Relay.HandleMessage, handleSignalingForward.
Rufannahme ist noch kein Nachweis einer funktionierenden Medienverbindung. Der Pi setzt seinen Telefonzustand bereits bei Annahme auf CONNECTED, überwacht aber zusätzlich die tatsächliche WebRTC-Verbindung und beendet einen fehlgeschlagenen Aufbau mit reason: "connect_timeout". Referenzen: Controller.onHookOff/onSignalAnswer in [controller.go](/Users/marcomhopp/Documents/GitHub/digits/pi/digitsd/internal/phone/controller.go), armConnectTimerLocked/connectDeadline.
Bei ICE-Restarts kommt das Angebot als ice_restart, die Antwort hingegen als sdp. Ein Signaling-Abbruch muss außerdem nicht sofort den laufenden Medienruf beenden: Der lokale Servercode hält Zweiergespräche für eine 20-Sekunden-Wiederverbindungsfrist offen. Referenzen: handleSignal, Relay.OnDisconnect/OnReconnect, graceWindow.
5. Audio und Codecs
Der tatsächlich gesendete Audiotrack verwendet Opus. Der Pi konfiguriert:
- 48 kHz, mono;
- 20-ms-Frames mit 960 Samples;
- 24 kbit/s Zielbitrate;
- VoIP-Modus, Complexity 5, In-Band-FEC und DTX.
Referenzen: newPeerManager/SendPCMFrame in [peer.go](/Users/marcomhopp/Documents/GitHub/digits/pi/digitsd/internal/webrtc/peer.go), codec.NewEncoder und FrameSize in [opus.go](/Users/marcomhopp/Documents/GitHub/digits/pi/digitsd/internal/codec/opus.go).
Dabei registriert die PeerConnection Pions Standard-Codecs; das Projekt beschränkt das gesamte SDP-Angebot nicht explizit auf Opus. Die genaue angebotene Codec-Liste, Payload-Typen und fmtp-Parameter sollten deshalb an einem realen SDP überprüft werden.
Die analoge Pi-Audiokonfiguration verwendet TLV320AIC3104 oder DA7212 über ALSA. Beim TLV320 läuft die Hardware laut Implementierung mit 44,1 kHz und ALSA resampelt für die 48-kHz-Pipeline. Diese Konfiguration ist keine Vorlage für ES8311/ES7210. Referenz: detectCodec in [alsa.go](/Users/marcomhopp/Documents/GitHub/digits/pi/digitsd/internal/audio/alsa.go).
6. Was auf ESP32 ersetzt werden muss
Die bestehende Go-Anwendung mit Pion, Gorilla WebSocket, Linux-Serial und CGO-Audiobindings kann nicht unverändert als ESP-IDF-Firmware laufen. Referenzen: [go.mod](/Users/marcomhopp/Documents/GitHub/digits/pi/digitsd/go.mod), [alsa.go](/Users/marcomhopp/Documents/GitHub/digits/pi/digitsd/internal/audio/alsa.go), [rnnoise_cgo.go](/Users/marcomhopp/Documents/GitHub/digits/pi/digitsd/internal/audio/rnnoise_cgo.go).
Neu benötigt werden:
- Digits-kompatibler JSON-/WebSocket-Adapter mit Pairing, Token und Wiederverbindung;
- WebRTC-Anbindung an ESP-WebRTC/esp_peer;
- Audioaufnahme und Wiedergabe über I2S und die Waveshare-Codecs;
- vereinfachte Telefonzustandsmaschine mit Hook und direkten Ruftasten;
- persistente Konfiguration und WLAN-Einrichtung.
Übernommen werden können Protokoll und Verhalten, nicht die Linux-Implementierung. Pico-UART, Matrix-Tastatur, systemd, ALSA und Pi-Update-/Recovery-Verfahren entfallen beziehungsweise erhalten eigene ESP32-Lösungen.
7. Wahrscheinliche Kompatibilitätsrisiken
- Versionsabweichung: Die beschriebenen Details stammen aus dem Checkout. Vor Implementierung müssen sie gegen v1.99.0 abgeglichen werden.
- SDP/ICE-Abbildung: Das Digits-Envelope enthält keinen expliziten SDP-Typ und für ICE weder sdpMid noch sdpMLineIndex. Der Adapter muss die Rollen und den Audiostream korrekt zuordnen. Referenzen: Message, PeerManager.AddICECandidate.
- Reihenfolge und Zwischenzustände: call muss vor SDP/ICE kommen; Kandidaten während des Klingelns müssen gespeichert werden. Abheben vor vollständigem Angebot und verspätete Nachrichten nach Auflegen benötigen definierte Behandlung. Referenzen: InitiateCall, AnswerCall, handleSignal.
- Opus-/DTLS-/SRTP-Interoperabilität: Sie ist anhand des Go-Codes allein nicht bewiesen. Auch Speicherbedarf, CPU-Auslastung und Audiopuffer des ESP32 bleiben zu messen.
- Tokenverlust beim Pairing: Die Datenbankzuordnung erfolgt vor der Zustellung von paired. Scheitert Zustellung oder Speicherung, bleibt das Gerät serverseitig gekoppelt, besitzt aber kein gültiges Token. Referenzen: handlePhonesPairPost, Handler.handleWS.
- Zusätzliche Serverfunktionen: Einstellungen und Pi-spezifische Verwaltungsbefehle müssen bewusst behandelt werden. Gruppenrufe sind bereits vorhanden, bringen jedoch zusätzliche Mesh-Peers und Zustände mit. Referenzen: Relay.OnRegistered, [conference.go](/Users/marcomhopp/Documents/GitHub/digits/server/internal/signaling/conference.go).
Die TH-07-Pinbelegung und elektrische Eignung bleiben ausdrücklich offen; dazu lässt sich aus dieser Softwareanalyse nichts ableiten.
8. Kleinster erster ESP32-Meilenstein
Ich empfehle zunächst einen reinen Signaling-Prototyp auf dem Waveshare-Board:
1. WLAN-Verbindung und ws://…/ws;
2. persistente UUID und korrektes register;
3. Pairing-Code über serielle Ausgabe;
4. Kopplung über die vorhandene Digits-Weboberfläche;
5. Token und Rufnummer speichern;
6. nach Neustart authentifiziert verbinden und Ping/Pong zuverlässig bedienen.
Erfolgskriterium: Das ESP32 erscheint nach Pairing und Neustart unter seiner zugewiesenen Leitung online, ohne Serveränderung.
Danach folgt unmittelbar der Interoperabilitätsnachweis mit einem bestehenden Pi/Pion-Client: ein Zweiergespräch im LAN, zunächst mit Testaudio, erfolgreichem ICE/DTLS/SRTP-Aufbau und sauberem Rufende. Erst dieser Test belegt die zentrale technische Machbarkeit. Die endgültige Hörerbeschaltung, Gruppenrufe und Gehäuse bleiben danach angesiedelt.