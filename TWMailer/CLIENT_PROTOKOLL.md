# TW-Mailer: Vereinbarung für die Client-Implementierung

Der Client wird separat entwickelt. Serverstart: `./twmailer-server <port> <mailspool>`; Clientstart: `./twmailer-client <ip> <port>`.

**TCP ist ein Bytestrom.** `send()`/`recv()` können weniger Bytes übertragen als erwartet. Zeilen enden mit `\n`. Server sendet **keine Begrüßung**. Alle Angaben sind UTF-8/ASCII-kompatibler Klartext; Benutzernamen nur `a-z0-9`, Länge 1–8, Betreff max. 80 Bytes (im Aufgabenblatt: Zeichen).

## SEND
Client sendet vollständig:
```
SEND
max
anna
Test
Hallo!
.
```
Dann genau eine Antwortzeile lesen: `OK` oder `ERR`. Der einzelne Punkt auf eigener Zeile beendet die Nachricht; im Nachrichtenkörper ist eine alleinstehende Punktzeile daher nicht darstellbar.

## LIST
Client sendet `LIST\nanna\n`. Server antwortet **ohne OK/ERR**: zunächst eine Dezimalzahl `N\n`, anschließend genau N Betreffzeilen. Unbekannter Benutzer -> `0\n`. Nachrichtennummern beginnen bei 1 und entsprechen der Reihenfolge der Betreffzeilen.

## READ
Client sendet `READ\nanna\n1\n`. Server antwortet `ERR\n` oder `OK\n` gefolgt von Sender, Empfänger, Betreff, Nachrichtenzeilen und `\.\n`. Nach `OK` bis zur einzelnen Punktzeile lesen.

## DEL
Client sendet `DEL\nanna\n1\n`. Server antwortet `OK\n` oder `ERR\n`. Nach dem Löschen können sich die Nummern in LIST/READ/DEL verschieben.

## QUIT
Client sendet `QUIT\n`; der Server schließt die Verbindung **ohne Antwort**.

## Wichtige Zusammenarbeit
- Port und IP-Adresse müssen zusammenpassen. Auf demselben PC: `127.0.0.1`, Port `2525`.
- Für SEND keine Antwort nach einzelnen Feldern erwarten, sondern erst nach `\.\n`.
- LIST beginnt mit der Anzahl, nicht mit `OK`.
- READ beginnt mit Status, bei OK endet die Nachricht mit Punktzeile.
- Kein Login, keine TLS-Verschlüsselung, keine zusätzlichen Protokollfelder.
- Die interne Speicherung (`mailspool`) ist ausschließlich Sache des Servers.