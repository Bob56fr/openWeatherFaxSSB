# Contribuer à openWeatherFaxSSB

Merci de votre intérêt ! Les issues, discussions et pull requests sont bienvenues **en français ou en anglais**.
*Contributions are welcome in French or English. The rules below are summarised in English at the end.*

## Ce qui aide le plus

- **Enregistrements WAV de réceptions réelles** (bonnes et mauvaises), pour le corpus de tests de non-régression (spec TST-001). Indiquez la station, la fréquence, la date et l'heure UTC, le récepteur et la source audio.
- **Horaires et fréquences de stations** à jour (catalogue).
- **Retours sur du vieux matériel** : modèle, version d'Android ou de Windows, ce qui marche et ce qui ne marche pas.
- **Traductions.**
- **Code** : voir les issues étiquetées `good first issue`.

## Avant de coder

1. Lisez la [spécification](docs/SPECIFICATION.md). Chaque exigence a un identifiant (`DSP-005`, `STA-020`…).
2. Pour toute fonctionnalité nouvelle ou tout changement d'architecture, ouvrez d'abord une issue.
3. Citez les identifiants d'exigences concernés dans l'issue et dans la PR.

## Certificat d'origine du développeur (DCO)

Le projet n'utilise **pas** de CLA. À la place, chaque commit doit porter une ligne de signature, par laquelle vous certifiez avoir le droit de soumettre votre contribution sous la licence du projet (AGPL-3.0-or-later). Le texte complet est dans le fichier [DCO](DCO).

```
Signed-off-by: Prénom Nom <adresse@exemple.org>
```

Git l'ajoute automatiquement avec l'option `-s` :

```sh
git commit -s -m "dsp: correction automatique du slant (DSP-005)"
```

Vous conservez vos droits d'auteur sur votre contribution. Un contrôle automatique (workflow `DCO`) bloque toute PR contenant un commit non signé.

**Commit oublié ?** Pour signer tous les commits de votre branche :

```sh
git rebase --signoff origin/main
git push --force-with-lease
```

Le nom et l'adresse doivent être réels ou correspondre à votre identité habituelle sur GitHub. L'adresse `noreply` de GitHub est acceptée.

## Règles techniques

### Code C++ (cœur)

- C++17 strict, sans extension compilateur. Le code doit compiler pour **toutes** les cibles de la CI : Linux amd64, arm64 et armhf ; Windows x64 et x86 ; Android API 21 ; Debian 11 avec CMake 3.18.
- **Windows 7** : aucune API Windows postérieure à Windows 7 (`_WIN32_WINNT=0x0601` est imposé).
- **Vieux matériel** : pensez au Pi Zero 2 W (512 Mo, Cortex-A53) et aux téléphones Android 5. Toute fonction gourmande doit être désactivable selon le niveau de matériel (spec §2.2).
- Aucun avertissement : la CI compile avec `-Werror` ou `/WX`.
- Formatage : `clang-format` avec le fichier [.clang-format](.clang-format), vérifié par la CI.
- En-tête obligatoire en tête de chaque fichier source :
  ```cpp
  // SPDX-License-Identifier: AGPL-3.0-or-later
  // Copyright (C) 2026 openWeatherFaxSSB contributors
  ```
- Les commentaires et identifiants du code sont en anglais.

### Dépendances

Toute nouvelle dépendance doit être **compatible AGPL-3.0** et justifiée dans la PR. Licences acceptées : MIT, BSD, Apache-2.0, Boost, zlib, LGPL, GPL-2.0-or-later, GPL-3.0, AGPL-3.0. **Refusées** : GPL-2.0-only, licences propriétaires (dont l'API SDRplay), « non commercial ».

### Règles non négociables

- **Réception seule** : aucun code ne doit pouvoir déclencher une émission (PTT, TX, tuner) — spec NFR-001, CAT-002.
- **Intégrité météo** : aucun traitement ne doit pouvoir ajouter une information absente du signal ; l'image brute est toujours conservée — spec DSP-030.
- **Aucune télémétrie** — spec NFR-005.

### Tests

- Toute correction de bug est accompagnée d'un test qui la reproduit.
- Tout changement du décodeur doit maintenir ou améliorer les métriques sur le corpus WAV (spec TST-002).
- Lancer localement : `ctest --test-dir build --output-on-failure`.

## Messages de commit

```
<zone>: <résumé à l'impératif, ≤ 72 caractères>

<explication du pourquoi, références d'exigences, issue>

Signed-off-by: Prénom Nom <adresse@exemple.org>
```

Zones usuelles : `core`, `dsp`, `wefax`, `rtty`, `navtex`, `sdr`, `cat`, `web`, `android`, `station`, `catalog`, `build`, `ci`, `docs`.

## Processus de PR

1. Forkez le dépôt et créez une branche depuis `main`.
2. Commits signés, CI verte, PR liée à une issue.
3. Au moins une revue approuvée avant fusion.

## Signaler une faille de sécurité

Ne pas ouvrir d'issue publique. Utilisez *Security → Report a vulnerability* sur GitHub.

---

## English summary

- No CLA. Every commit must be signed off under the [Developer Certificate of Origin](DCO): use `git commit -s`. A CI check enforces it.
- C++17, warning-free on all CI targets (including Windows 7 API level and Android API 21). Formatting is checked with `clang-format`.
- New dependencies must be AGPL-3.0 compatible. GPL-2.0-only and proprietary code (e.g. the SDRplay API) are not accepted.
- Hard rules: receive-only (never transmit), never alter the weather information (raw image always kept), no telemetry.
- Reference real WAV recordings and spec requirement IDs whenever possible.
