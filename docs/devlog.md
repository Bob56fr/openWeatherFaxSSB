# Journal de développement

Notes de travail, du plus récent au plus ancien. Heures en UTC (Paris = UTC+1 en hiver).

## Nuit du 9 au 10 novembre 2026 — travail autonome (4 étapes)

Cadre convenu : pas d'accès radio hors des sessions cron, pas de changement de
spécification, commits signés sur `main` avec CI verte, seule publication : la
release `corpus-v1` (validée).

### Étape 2 — découpage des émissions GYA ✅

- Constat : la carte GYA de 23:12 n'était pas séparée de la précédente.
- Cause mesurée : le ton APT de début (300 Hz, ~4 s) arrivait haché par le QRM en
  morceaux d'~1 s séparés de trous de 0,25 s ; le détecteur exigeait 2 s d'affilée.
- Correction : un trou isolé de 0,25 s est toléré, 2 s de ton cumulé restent exigées.
  Test synthétique ajouté. La carte de 23:12 est maintenant trouvée (1292 lignes).

### Étape 3 — suppression des clics FM (DSP-020) ✅

- Option `--clicks` (désactivée par défaut : l'image brute reste la référence, DSP-030).
- Synthétique avec QRN impulsif fort : corrélation 0,858 → 0,940 ; image propre intacte.
- Carte DWD réelle de 22:00 : points noirs isolés 273 → 108 (−60 %) ; lignes striées
  inchangées (33 → 35) — elles viennent des creux de fading (baisse du rapport
  signal/bruit), pas de clics. Prochaine piste : réparation de lignes (DSP-021) et
  diversité de fréquence (DSP-040).
- L'indicateur de qualité par ligne est désormais calculé sur le signal reçu, avant
  toute correction, pour ne jamais masquer une mauvaise réception.

### Analyse des stries (carte DWD 22:00)

| | Lignes striées (33) | Lignes propres |
|---|---|---|
| Échantillons hors bande | 15,5 % | 3,1 % |
| Niveau médian | −20,4 dB | −15,1 dB |

~40 % en creux de fading > 6 dB, ~27 % avec impulsion ; lignes isolées → réparables
par interpolation verticale.

## Questions pour toi

_(à compléter au fil de la nuit)_
