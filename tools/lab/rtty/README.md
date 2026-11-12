# POC RTTY météo + SYNOP

Preuve de concept **hors produit**. Elle sert à juger si les bulletins RTTY du DWD
(texte marine, observations SYNOP/SHIP) apportent quelque chose à côté des cartes
WEFAX. Elle n'est pas intégrée au cœur C++ et peut être supprimée sans impact. Elle
prépare TXT-001 et TXT-003 de la spécification.

| Fichier | Rôle |
|---|---|
| `rtty.py` | WAV 16 bits → texte ITA2 (50 Bd, shift 450 Hz par défaut). Recherche automatique de la paire de tonalités et de la polarité. Python + numpy |
| `synop.py` | Texte → observations SYNOP (FM 12) / SHIP (FM 13) en clair (français) ou JSON. Bibliothèque standard seulement |
| `forecast.py` | Texte → prévisions par point DWD (FEMM74, FEAE75, FEEN73, FEBQ72, FQMM79…) : zone, position, SST, puis par échéance vent/rafales (Bft), vagues (m), temps. Signale les zones où la force atteint 8 Bft. Une ligne ou un en-tête abîmé est mis à part, jamais deviné |
| `test_rtty.py` | Signal FSK synthétique bruité portant un bulletin, décodé puis analysé : `python test_rtty.py` |

## Capture (station minix, IC-7300)

Le script de capture WEFAX convient tel quel. Avec la fréquence affichée à
f − 1,9 kHz, les tonalités tombent vers 1675 et 2125 Hz, dans la bande passante PKTUSB de 3 kHz :

```sh
tools/capture/ic7300_capture.sh 7646 30 DDH7-rtty   # 30 min sur DDH7
```

Programme anglais (« RTTY Broadcast for Shipping, Prog. 1 », DWD, édition 05/2014,
encore exact le 2026-11-11) : DDK2 4583 kHz (1 kW), DDH7 7646 kHz (1 kW),
DDK9 10100,8 kHz (10 kW), 24 h/24, 50 Bd, ±225 Hz. Sur DDH7, la tonalité mark est
la plus basse. Créneaux utiles (UTC) :

- SYNOP : 00:35, 06:30, 12:10, 18:30 (environ 1 h)
- SHIP : 02:00, 04:40, 07:35, 10:35, 13:35, 16:35, 19:35, 22:35 ; BUOY : 08:15, 20:15
- prévisions par point sur 5 jours : FEMM74 (Méditerranée) 04:15/14:13/16:10,
  FEAE75 (Atlantique Est) 06:04/18:04, FEEN73 (mer du Nord), FEBQ72 (Baltique)
- FQEW77 (Western European Sea) 09:55/21:55, FQMM78 (Méditerranée occidentale) 10:15/22:15

Mesure du 2026-11-11 sur DDH7 : avec le simple discriminateur (hi−lo)/(hi+lo), 7,6 %
des trames sont fausses, à cause de l'écho multitrajet et du fading sélectif. Avec la
correction de seuil par tonalité et une intégration sur 0,75 bit, on descend à 0,2 %.

Résultats du 2026-11-11 et du 2026-11-12. La fréquence choisie selon l'heure compte plus
que le traitement du signal :

| Bulletin | Fréquence, heure (UTC) | Zones lisibles | Lignes rejetées |
|---|---|---|---|
| FQEW77 | 10100,8 kHz, 09:50 | 14/14 | 0 % |
| FEAE75 | 7646 kHz, 18:00 | 20/20 | 1 % |
| FQEW77 | 7646 kHz, 21:50 | 5/11 | 44 % |
| FEAE75 | 7646 kHz, 06:00 | – | 70 % |

De jour, il faut 10100,8 kHz. Entre 16 et 19 UTC, 7646 kHz convient. La nuit et à l'aube,
7646 kHz est mauvais (4583 kHz reste à essayer). Les SYNOP et SHIP sont aujourd'hui presque
vides (quelques stations danoises et polonaises, une dizaine de navires) : l'intérêt est
dans les prévisions par point FE../FQ...

## Décodage

```sh
python rtty.py capture.wav --timestamps -o capture.txt   # stats sur stderr
python synop.py capture.txt                              # observations en clair
python synop.py capture.txt --json                       # pour exploitation
python forecast.py capture.txt                           # prévisions par point (--json)
```

Options utiles de `rtty.py` : `--center` (si la recherche se trompe de signal),
`--mark high|low`, `--squelch` (0,3 par défaut : en dessous, les caractères sont ignorés), `--usos`.
Si le `.json` de capture est présent à côté du WAV, l'horodatage est en UTC.

Le Baudot n'a aucun contrôle d'erreur. Tout groupe SYNOP illisible ou toute valeur non
plausible est marqué `[DOUTEUX : …]` au lieu d'être présenté comme sûr.
