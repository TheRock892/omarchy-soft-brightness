# omarchy-soft-brightness

Luminosità "virtuale" per un monitor esterno quando il vero DDC/CI non funziona.

Niente di nuovo lato hardware: disegna un rettangolo nero semi-trasparente, a tutto schermo, **solo sul monitor scelto**, trasparente ai click/tastiera (come facevano alcune estensioni GNOME quando DDC non era disponibile). Funziona su qualunque compositor Wayland che implementa `wlr-layer-shell` (Hyprland, quindi Omarchy, incluso).

## Installazione

Installa il plugin della barra (clona questo repo in `~/.config/omarchy/plugins/`) e poi compila il demone/CLI da cui dipende:

```
omarchy plugin add https://github.com/TheRock892/omarchy-soft-brightness --enable
sh ~/.config/omarchy/plugins/io.github.therock892.soft-brightness/install.sh
```

Il secondo comando compila ed installa `omarchy-soft-brightnessd` e `omarchy-soft-brightness` in `~/.local/bin`. Nessun permesso di root, nessun servizio di sistema: è un normale client Wayland dell'utente.

Dipendenze richieste in compilazione: `gcc`, `pkg-config`, `wayland-scanner`, header di sviluppo `wayland-client`/`wayland-protocols`.

Per usare solo lo strumento da riga di comando (senza il plugin della barra), clona il repo altrove e lancia `sh install.sh` da lì.

## Rimozione

```
omarchy plugin remove io.github.therock892.soft-brightness --yes
rm -f ~/.local/bin/omarchy-soft-brightness ~/.local/bin/omarchy-soft-brightnessd
rm -rf ~/.local/state/omarchy-soft-brightness
```

## Uso

```
omarchy-soft-brightness status        # livello attuale (0-100, 100 = luminosità piena, 0 = più scuro)
omarchy-soft-brightness 50%           # imposta al 50% di luminosità
omarchy-soft-brightness +10%          # aumenta di 10 punti
omarchy-soft-brightness 10%-          # diminuisce di 10 punti
omarchy-soft-brightness off           # nessun oscuramento (equivalente a 100%)
omarchy-soft-brightness --monitor HDMI-A-1 50%   # forza un monitor specifico
```

La percentuale rappresenta la luminosità desiderata, non quanto oscurare: 100% = nessun effetto (luminosità piena). Il minimo selezionabile è **10%** (non si arriva mai a schermo completamente nero, impostato sia nel demone che nello script).

## Plugin per la barra di Omarchy

Installato come plugin della barra di Omarchy (`~/.config/omarchy/plugins/io.github.therock892.soft-brightness/`): icona nella barra con popup a slider (10-100%), rotellina del mouse per variare di 5 punti, auto-refresh mentre il popup è aperto. È solo un'interfaccia sopra lo script `omarchy-soft-brightness` — non parla mai direttamente al socket del demone.

Comandi utili se lo si modifica:
```
omarchy-shell shell rescanPlugins   # ricarica dopo modifiche al Panel.qml
omarchy plugin enable io.github.therock892.soft-brightness
omarchy plugin disable io.github.therock892.soft-brightness
```

Senza `--monitor`, individua automaticamente il primo monitor esterno collegato (qualsiasi non `eDP`/`LVDS`/`DSI`) tramite `hyprctl monitors`.

Il demone (`omarchy-soft-brightnessd`) si avvia da solo al primo comando e resta in esecuzione finché il monitor non viene scollegato o non si invia `QUIT`. Il livello scelto è persistito in `~/.local/state/omarchy-soft-brightness/<monitor>.level` e ripristinato automaticamente al prossimo avvio del demone.

## Collegarlo ai tasti Fn

Per usare gli stessi tasti luminosità (Fn+F5/F6) anche sul monitor esterno quando è collegato, basta aggiungere un bind in Hyprland che richiama questo script al posto di (o in aggiunta a) `omarchy-brightness-display`, puntando esplicitamente al monitor esterno.

## Limiti conosciuti

- È un trucco visivo (oscura con un overlay nero), non modifica la vera retroilluminazione: a livelli alti di oscuramento il contrasto e la qualità percepita dell'immagine calano, come con qualsiasi tecnica gamma/overlay software (stesso compromesso che aveva l'estensione GNOME equivalente).
- Se il monitor viene scollegato e ricollegato, il demone esistente termina (evento `closed` del protocollo) e va semplicemente richiamato di nuovo il comando: si auto-rialza.
- Un solo demone per monitor: comandi concorrenti sullo stesso monitor sono serializzati dal socket di controllo, nessun problema per l'uso normale (tasti, script).

## Licenza

MIT, vedi `LICENSE`.
