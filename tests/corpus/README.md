# Corpus de réceptions réelles

Enregistrements radio réels servant aux tests de non-régression du décodeur
(spec TST-001, TST-002). Chaque entrée est une émission complète, rééchantillonnée
à 11 025 Hz (WAV mono 16 bits), avec son décodage de référence.

- **Manifeste** : [corpus.tsv](corpus.tsv) — fichier, SHA-256, station, fréquence,
  heure UTC, et les métriques de référence (lignes, erreur d'horloge, calage, qualité).
- **Fichiers** : release GitHub [`corpus-v1`](https://github.com/Bob56fr/openWeatherFaxSSB/releases/tag/corpus-v1)
  (hors du dépôt Git).
- **Récupérer** : `tools/corpus/fetch.sh` (vérifie les empreintes), puis
  `OWFAX_CORPUS_DIR=tests/corpus/data ./build/tests/owfax_tests`.
- **Ajouter** : `tools/corpus/extract.sh <capture.wav> <dossier>` sur la station de
  réception, puis publier les fichiers dans la release et ajouter les lignes au manifeste.

Le test `corpus_real_receptions_no_regression` exige que chaque émission se décode au
moins aussi bien que sa référence. Améliorer le DSP doit faire progresser ces chiffres ;
les mettre à jour est un choix explicite, dans le même commit que l'amélioration.

## Source des données et conditions de réutilisation

Les signaux sont des diffusions radiofax publiques pour la navigation du
**Deutscher Wetterdienst (DWD)**, émetteurs de Hamburg/Pinneberg (DDH3 3855 kHz,
DDK3 7880 kHz, DDK6 13882,5 kHz), reçus avec un Icom IC-7300MK2.

- Enregistrements audio : **Quelle : Deutscher Wetterdienst** (diffusion radiofax, enregistrement non modifié hormis découpage et rééchantillonnage).
- Images décodées : **Datenbasis : Deutscher Wetterdienst, Funkaufzeichnung und Dekodierung durch openWeatherFaxSSB (bearbeitet)**.

Réutilisation selon les conditions du DWD (GeoNutzV, mention de la source obligatoire et
signalement des modifications) : <https://www.dwd.de/copyright>. Ces conditions n'ont pas
été confirmées spécifiquement pour les diffusions radiofax ; en cas de demande du DWD, les
fichiers seront retirés. Ces données ne relèvent pas de la licence AGPL du code.

Les cartes reçues sont des archives de test : **ne pas les utiliser pour la navigation**.
