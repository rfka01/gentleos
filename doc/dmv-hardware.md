# DMV: Boot, Timing und Bildschirm

Belegte Hardwarefakten zur NCR Decision Mate V, gesammelt bei der
DOS-3.3-Portierung und beim GentleOS/16-Port. Quellen jeweils genannt.
Gedacht als Grundlage für jedes eigene System auf dieser Maschine
(DOS-Portierung, GentleOS/16, eigene Programme) und für die MAME-Emulation.

## 1. Die Interruptvektortabelle muss gefüllt werden, bevor irgendetwas läuft

NCRs eigenes BIOS aus MS-DOS 2.11 (Quellpaket `DM5SOURC`, `basinit.asm`)
macht als allererstes nach `CLI`:

```
HWINIT:
	CLI
	XOR     BX,BX
	MOV	DS,BX			;DS = ZERO
	MOV	ES,BX
	CLD
	MOV	WORD PTR [BX],OFFSET INT_TRAP
	INC	BX
	INC	BX
	MOV	WORD PTR [BX],CS
	MOV	DI,4
	XOR	SI,SI
	MOV	CX,510
	REP	MOVSW
```

Vektor 0 auf `INT_TRAP`, dann diesen Eintrag über die ganze Tabelle
kopiert (selbstüberlappendes MOVSW). `INT_TRAP` ist in `iobase.asm` ein
nacktes `IRET`:

```
; INT_TRAP
; DOES INTERRUPT RETURN FOR ALL UNSPECIFIED VECTORS
```

Erst danach setzt NCR seine eigenen Vektoren, und zwar genau sechs:
08h Timer, 12h Speichergröße, 14h RS232, 16h Tastatur, 1Ch
Timer-Rückgabe, 29h schnelle Zeichenausgabe.

Auf der DMV steht in der Vektortabelle beim Start Müll. Jeder nicht
besetzte `INT` springt ins Leere, und man sieht das zero-sled-Muster
(CPU frisst sich durch `00 00` = `add [bx+si],al`), das wie ein Neustart
aussieht, während das RAM intakt bleibt.

Belegter Fall: DR Draws GSX-Bildschirmtreiber `DDNCR.SYS` löst beim
Umschalten in die Grafik `INT 0DEh` aus. In Digital Researchs eigenem
Treiberquelltext `DDDMV2.A86` steht dazu:

```
SBDOS	EQU	222		;SPEZIAL OUTPUT TO CPM OPERATING SYSTEM
```

Ein Concurrent-CP/M-Aufruf, den MS-DOS nicht kennt. CL ist die Funktion
(0 Bildschirm, 1 Tastatur, 2 "welcher Bildschirm ist im Vordergrund"),
AL ist 0FFh für ein und 00h für aus. Unter 2.11 landet der Aufruf auf
dem IRET-Trap, AL bleibt 0FFh - und das ist zufällig die richtige
Antwort ("du bist vorn"). Ohne gefüllte Tabelle stürzt dieselbe Software
ab.

Regel: Tabelle komplett mit IRET füllen, dann die eigenen Vektoren
setzen.

**GentleOS/16-Umsetzung (nativer Boot):** Der Kernel-Start füllt die
gesamte IVT (256 Einträge) mit einem eigenen `IRET`-Handler, bevor
irgendetwas einen Interrupt auslösen kann. Das war die Ursache der
früheren Reboot-Schleife: der erste `INT 10h` sprang durch einen
Müllvektor.

## 2. Zeitbasis und Interruptcontroller

Aus `basinit.asm`, direkt nach der Vektortabelle:

```
; 8259A, einkanalig
	MOV	AL,17H		;EDGE TRIG, ADDRESS SPACE 4, SINGLE, ICW4
	OUT	90H,AL
	MOV	AL,8		;ICW2: INT TYPE 8 FOR IRQ0
	OUT	91H,AL
	MOV	AL,3		;ICW4: AEOI, 8088 MODE
	OUT	91H,AL

; 8253 Kanal 2 = Systemtakt
	MOV	AL,0B6H		;Kanal 2, LSB+MSB, Modus 3
	OUT	83H,AL
	MOV	AX,5000		;5000 * 500 KHZ = 10 MS
	OUT	82H,AL
	XCHG	AH,AL
	OUT	82H,AL
```

- 8259 auf 90h (Kommando) / 91h (Daten), Single-Modus, AEOI. Kein EOI
  nötig, kein Slave. Vektorbasis 8, IRQ0 ist INT 08h. NCR schreibt keine
  Maske; wer IRQ0 will, schreibt anschließend OCW1 auf 91h (0FEh = nur
  IRQ0 frei).
- 8253 Kanal 2 auf 82h (Daten) / 83h (Steuerwort). Eingangstakt
  **500 kHz**. NCR teilt durch 5000, also 10 ms. PC-kompatible
  18,2065 Hz ergeben 27463.
- Die DMV hat keinen PC-kompatiblen Vorteiler. Jede Rechnung mit
  1193182 Hz ist hier falsch.

### K230 und K235: nicht jede 8088-Karte bekommt den Timer-Interrupt

Der 8253 sitzt auf der Hauptplatine; sein Kanal-2-Ausgang (TIMINT) geht
nur an den Steckplatz der CPU-Karte. Belegt in MAME (`bus/dmv/k230.cpp`,
`ncr/dmv.cpp`):

- **K235** - 8088/V20 *mit* 8259 auf 90h/91h. TIMINT wird IRQ0 -> INT 08h.
- **K230** - 8088 *ohne* Interruptcontroller. TIMINT geht ins Leere; auf
  90h/91h antwortet nichts (auf der Hauptplatine unbelegt, Schreiben ist
  harmlos). Ein `HLT` wartet auf einer K230 für immer.

Den 8253 können beide Karten lesen (82h/83h). Auf einer K230 lässt sich
die Zeit daher nur durch Abfragen des Zählers messen.

**GentleOS/16-Umsetzung (nativer Boot):** Der Timer startet den
10-ms-Takt, gibt Interrupts frei und beobachtet Kanal 2 etwa 40 ms lang.
Kommt in der Zeit ein Timer-Interrupt, ist es eine K235 und es läuft
interruptgesteuert (die CPU darf `HLT`). Kommt keiner, ist es eine K230:
Interrupts bleiben aus, Kanal 2 läuft frei in Modus 2 über die vollen
16 Bit (131 ms je Umlauf), und die Leerlaufschleife leitet die Ticks aus
dem Zählerstand ab, statt `HLT` auszuführen.

## 3. Boot-Übergabe Z80 -> 8088

- Das Z80-Mainboard-ROM übergibt nur, wenn das OEM-Feld des Bootsektors
  (Bytes 3-10) exakt `16BIT` mit Füllleerzeichen ist.
- Der Bootsektor wird nach 0000:2000 geladen und dort angesprungen.
- Postfach im RAM (Firmware-Liste, Systemhandbuch Anhang A):
  - `0:FE01` SB8 - Status vom 8-Bit-Teil
  - `0:FE04` CB16 - Kommando an den 8088 (0 Monitor, 1 STARTS,
    2 EXECUTE_SOFTWARE, 3 LEVEL0; ab 4 zurück auf den Z80)
  - `0:FE05` SB16 - Status zurück (0 gut, 0FFh Fehler)
  - `0:FE06` MEM16 - Anzahl der 64-KB-Erweiterungsbänke, ohne die
    64 KB der Hauptplatine
- Das 4-KB-ROM der 8088-Karte liegt 128-fach gespiegelt über
  80000h-FFFFFh. Am ROM-Anfang steht `JMP DISPLAY`, deshalb funktioniert
  `FF00:0000` für Textausgabe: BX zeigt auf einen Zählstring (Längenbyte,
  dann Text), gelesen über DS - also DS auf 0 setzen und absolut
  adressieren.
- Umschaltung: `OUT 11h` ROMSEL (Karten-ROM über 0000h-1FFFh, acht
  Kilobyte), `OUT 10h` RAMSEL zurück, `OUT 00h` Prozessorwechsel. Wer
  aus einer Adresse unter 2000h heraus umschaltet, holt den nächsten
  Befehl aus dem ROM.
- Ohne RAM-Erweiterung (K200/K202/K208) hat die 8088-Karte nur die 64 KB
  der Hauptplatine; Lesen darüber liefert 0FFh. GentleOS braucht
  Speicher ab 10000h (Kernel) und 30000h (initrd).
- Die Diskettenleistung ist durch die Drehzahl begrenzt: 9 Sektoren je
  Umdrehung bei 300 U/min sind ~23 KB/s. Multi-Track-Lesen (MT-Bit) bringt
  gegenüber kopfweisem Lesen nichts Messbares.

## 4. Tastatur (8741)

Aus `basinit.asm`:

```
COUNTRY       EQU 01H   ;Kommando: Landescode liefern
KBD_CMD_PORT  EQU 41H
KBD_STA_PORT  EQU 41H
KBD_DATA_PORT EQU 40H
KBD_DATA_RDY  EQU 01H   ;Statusbit 0: Zeichen da
KBD_LANG_VAR  EQU 80H   ;Statusbit 7: das Zeichen ist der Landescode
```

- `OUT 41h,1` ist zugleich die nötige Initialisierung; ohne sie liefert
  der 8741 gar nichts.
- **Antwort auf `01h` (in MAME gemessen):** Status mit Bit 7 gesetzt, das
  Datenbyte ist `E8h + Landescode`. Der Landescode (0-7) sind die
  DIP-Schalter unter der Tastatur; Version-1-Tastaturen: 0 US,
  1 UK/Int., 2 Dänisch, 3 Deutsch, 4 Schwedisch/Finnisch, 5 Norwegisch,
  6 Spanisch, 7 Italienisch. Version-2-Tastaturen (Schweiz, Frankreich,
  ...) belegen dieselben Codes anders und sind daran nicht zu erkennen.
- Die Tastatur sendet das Zeichen, das auf der Taste steht. Auf der
  deutschen Tastatur sitzt Y also dort, wo die US-Tastatur Z hat
  (QWERTZ); auf der italienischen sind W und Z vertauscht (QZERTY).
  Programme, die die Tastatur als Fläche benutzen (Klaviatur, Spiele),
  müssen das je nach Landescode zurückrechnen.
- Sondertasten kommen als 80h-9Fh. Die Eingabetaste ist **88h, nicht
  0Dh**; NCRs Tabelle `KBDTBL` setzt sie auf 0Dh um.
- 88h ist gleichzeitig der Bildschirm-Steuercode "neue Zeile" (NEWL) der
  Firmware. Dieselbe Zahl, zwei Bedeutungen.
- Die deutsche Tastatur sendet die ISO-646-Codes 7Bh/7Ch/7Dh und
  5Bh/5Ch/5Dh für die Umlaute; der Landescode steuert die Umsetztabelle.

### Töne über den 8741 (TONE, Befehl 06h)

Befehl 06h auf 41h, dann Tonbyte (n + 20h, n = 1-42, A 110 Hz bis
D 1175 Hz, f = 110 * 2^((n-1)/12)) und Längenbyte (Einheiten + 1Fh, eine
Einheit ~20,5 ms; Länge 0 = 256 Einheiten, vermeiden) auf 40h.

Fallstrick: Während der 8741 einen Ton spielt, liest er seinen
Eingangspuffer nicht - IBF (Statusbit 1) steht aber trotzdem schon wieder
auf 0. Mehrere Bytes direkt hintereinander überschreiben sich dann im
Puffer; der 8741 sieht ein Kommando ohne Daten und wartet ewig auf ein
Datenbyte, samt Tastatur. Sicheres Protokoll (wie Hopplers SndPump):
bei IBF = 0 nur 06h schreiben; erst wenn IBF danach wieder 0 ist (der 8741
hat das Kommando genommen und wartet aktiv), Ton- und Längenbyte
hintereinander. Ein begonnenes Kommando immer zu Ende senden.

## 5. GDC uPD7220

Aus der Firmware-Liste (`BOOTER_IO_DRIVER`):

```
GDCCOM EQU 0A1H   ;Kommando schreiben
GDCSTA EQU 0A0H   ;Status lesen
GDCPAR EQU 0A0H   ;Parameter schreiben
FIFULL EQU 02H    ;Statusbit 1: FIFO ist voll
```

Dazu aus der Messung an DR Draws Treiber: Statusbit 5 (20h) ist der
**Bildrücklauf (VSYNC)**. Grafiksoftware wartet darauf, bevor sie den
Bildspeicher anfasst:

```
	MOV	DX,0A0h
	IN	AL,DX
	TEST	AL,20h
	JZ	zurueck
```

Ein Emulator oder Prüfstand, der an A0h einen festen Wert liefert, lässt
jedes solche Programm endlos drehen - das sieht aus wie ein Absturz, ist
aber nur der fehlende Bildrücklauf.

Festwerte der Firmware: 80 Zeichen je Zeile, 25 Zeilen, CURSOR 49h,
MASK 4Ah, FIGS 4Ch, PRAM 70h, WDAT 20h (Wort, Replace), START/End-Idle
68h. Variablen: `0:F800` CURSX, `0:F801` CURSY, `0:F900/F901`
Cursoradresse, `0:F902` COAD (Pixelposition), `0:F903` SP1 (Anfang
Seite 1), `0:F906` LP12 (Länge Seite 1), `0:F90B` INVFLG (Attribut,
E8h = grün auf schwarz).

Beim schnellen Schreiben gelernt:

- **CURS lädt auch MASK.** Der dritte CURS-Parameter enthält die
  Punktadresse dAD, und der GDC setzt MASK = 1 << dAD. Nach jedem CURS
  muss MASK für Wortzugriffe wieder auf FFFFh, sonst schreibt WDAT nur
  das linke Pixel jedes Wortes.
- FIGS bleibt über WDAT hinweg stehen (nur DC fällt danach auf 0, die
  Richtung bleibt) - einmal FIGS DIR=2 DC=0 genügt für viele Zeilen.
- "FIFO nicht voll" (Bit 1 = 0) garantiert nur einen freien Platz;
  "FIFO leer" (Bit 2 = 1) garantiert 16. Für Blöcke auf "leer" warten.
- Die Übertragung ist auf der DMV durch die CPU begrenzt, nicht durch den
  GDC (in MAME ist die FIFO dabei nie voll). Eine Assembler-Schleife
  (Byte holen, XLAT-Bitumkehr, XOR, OUT) ist rund neunmal schneller als
  derselbe Ablauf in C mit Funktionsaufruf je Byte.

## 6. Festplatte WD1001 - Zeitverhalten

- Ports: C0h Daten, C2h Sektorzahl, C3h Sektornummer, C4h/C5h Zylinder,
  C6h SDH, C7h Status/Kommando. Statusbits: 80h BUSY, 40h READY,
  08h DRQ, 01h ERROR.
- Ohne Controller liest der Bus 0FFh, und darin ist BUSY gesetzt. Jede
  Warteschleife auf "BUSY fällt" läuft dann ewig. Lösung: den Controller
  einmal mit einem Schreib-Lese-Test suchen (55h auf C4h, AAh auf C3h,
  zurücklesen) und das Ergebnis merken; alle Warteschleifen mit
  Zeitgrenze.
- Unteres Nibble des Kommandos: nur bei RESTORE (1xh) und SEEK (7xh) ist
  es die Schrittweite (0 = Buffered Seek 35 us, 1-15 = 0,5 bis 7,5 ms).
  Bei READ (20h) und WRITE (30h) stehen dort Mehrsektor- und
  Interruptbits - eine dort hineingeschriebene Schrittweite verbiegt die
  Übertragung.
- NCRs eigener Treiber kennt genau drei Kommandos: 10h RESTORE mit
  Schrittweite 0, 20h READ, 30h WRITE. Kein SEEK, kein langsames
  RESTORE.

## 7. Mono-DMV

- Beide Maschinen gibt es: Mono mit Mainboard-ROM 33609 "M.07.00",
  Farbe mit 33610 "C.07.00". Beide mit Grafikkarte und Zeichengenerator
  76161.
- Erkennung zur Laufzeit über die Versionszeichenkette im
  Mainboard-ROM. **Am ROM (33609/33610, 8 KB) verifiziert:** Die
  Zeichenkette steht am ROM-Offset **0FF9h** als
  `08 20 'M'/'C' 2E 30 37 2E 30 30` (also ` M.07.00` bzw. ` C.07.00`).
  Das unterscheidende Byte ist **0FF9h**: `4Dh` ('M') = mono,
  `43h` ('C') = Farbe. (Die frühere Notiz "0FFAh" war um eins daneben;
  bei 0FFAh steht in beiden ROMs identisch `2E 30` = ".0".) Das ROM
  liegt zur Laufzeit 128-fach über 80000h-FFFFFh gespiegelt, das Byte
  also physisch bei 0FEFF9h (z. B. F000:EFF9 oder FEFF:0009).
- Zeichengenerator 76161: nur 128 Glyphen, Bit 7 des Codes wird
  ignoriert. ASCII auf 20h-7Fh, Akzentzeichen auf 00h-1Fh und damit
  gespiegelt auch auf 80h-9Fh. Dadurch sind Backslash, eckige und
  geschweifte Klammern gleichzeitig mit den Umlauten darstellbar.
  DMV-Codes (NCR CRTTBL/GERMANY in `basinit.asm`): ae=90h, oe=96h,
  ue=99h, Ae=80h, Oe=86h, Ue=89h, sz=9Eh. Für my gibt es keinen
  gesicherten Code.
- 16 Rasterzeilen je Zeichenzeile (CSRFORM-Parameter 1, Bits 4-0 =
  Zeilen minus eins, Bit 7 = Cursor sichtbar). Der zweite
  Zeichengenerator für Text im Grafikbereich sitzt auf der Hauptplatine
  im CPU-ROM-Bereich 1000h-1FFFh, ebenfalls 16 Byte je Zeichen,
  erreichbar über ROMSEL/RAMSEL.
- Halbe Helligkeit gibt es nur auf der Mono-Maschine (Attributbit 8 =
  Bit 0 im oberen Byte). Auf der Farbkarte ist sie laut Schaltplan und
  Foto nicht bestückt (E5 fehlt). Das Gehäuse bestätigt es: Mono hat
  vorne Helligkeit und Kontrast (Lautstärke hinten neben dem
  Tastaturstecker), die Farbmaschine vorne Helligkeit und Lautstärke,
  keinen Kontrastregler.
- Halbe Intensität wird als echte Hälfte modelliert (80h statt FFh) und
  gilt für alles, was in der Zelle leuchtet, Vorder- und Hintergrund.
  Nicht an einen Standardkontrast anpassen.
- Cursor: blinkender Unterstrich, nicht volles Feld.
- Attribute gibt es nur im Textbereich ("No attributes are possible in
  graphic area").
- Port A2h ist auf neueren Karten ein Zoom-Latch (Boot-ROM C.07.00
  benutzt ihn, das K235-ROM 33473 nicht). Parameter dorthin statt nach
  A0h ergibt zusätzlich waagrechten Zoom; auf alten Karten ist A2h
  vermutlich ein Spiegel von A0h und damit harmlos.
- Warnung aus der Dokumentation: keine Video-Timing-Parameter ändern
  (SYNC/VSYNC).

## 8. Wo die Quellen liegen

- `DM5SOURC211.zip` - NCRs vollständige MS-DOS-2.11-BIOS-Quellen für die
  DMV: `basinit.asm` (Hardware-Init, Landescodes, Zeichentabellen),
  `iobase.asm` (INT_TRAP, Gerätekopfzeilen, CRT-Tabellen), `crtcode.inc`
  (Bildschirm, auch Grafikzeichen), `kbdcrt.asm`, `kbdcode.inc`,
  `dskdrv.asm`, `flxpimc.asm`/`flxpimd.asm` (765), `widrv5.asm`/
  `widrv10.asm` (WD1001), `comdrv.asm`, `lpdrv.asm`, `timdrv.asm`,
  `config.asm`.
- Firmware-Liste des 16-Bit-Teils: Systemhandbuch Anhang A (Boot-Loader
  Level 0 und IO-Driver, mit allen Equates und Adressen).
- Festplatte `615-4-17_fmt20 - Daten`, Verzeichnis `DRAW86.DMV`:
  `DDDMV2.A86`, Digital Researchs Quelltext eines DMV-GSX-Treibers, mit
  den GDC-Zugriffen und den Concurrent-CP/M-Aufrufen im Klartext.
- ROM-Images (mono 33609, Farbe 33610, Zeichengenerator 76161,
  8741-Tastatur-MCU u.a.) im Projektarchiv `dmv.zip`.
- MAME-Treiber `ncr/dmv.cpp`, `ncr/dmv_keyb.cpp`, `bus/dmv/k230.cpp`
  (K230/K235), `devices/video/upd7220.cpp`. Für Tests: `-slot1 k208`
  (512 KB), `-slot7a k230` bzw. `k235`; Lua-Taps auf die I/O-Ports der
  CPU-Karte zeigen jeden GDC-, Tastatur- und DMA-Zugriff mit Zeitstempel.
