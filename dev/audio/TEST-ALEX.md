# Tester l'audio de Beamer (Mac ↔ PC)

Version installée sur les deux machines : `feat/audio` @ `7b83e71` (2026-11-08 23:18). L'appli Mac est signée
« Beamer Dev ». Les programmes audio tournent des deux côtés, aucun routage n'est actif.

## 1. À faire une fois (toi)

**Sur le Mac**
- Si le clavier ou la souris ne passent plus d'une machine à l'autre : Réglages Système > Confidentialité et
  sécurité > **Accessibilité** et **Surveillance de l'entrée** > réactiver Beamer. Les déploiements de cet
  après-midi, mal signés, ont pu effacer ces droits. C'est corrigé, ils ne sauteront plus.
- **Micro** : macOS le demande la première fois que le micro du Mac est envoyé vers une autre machine.
  Clique « Autoriser ». Aujourd'hui l'état est « pas encore demandé ».
- **Enregistrement audio du système** : macOS le demande la première fois que le son du Mac part vers une
  autre machine, ou que l'annulation d'écho démarre. Clique « Autoriser ».
- Dans tes applis d'appel sur le Mac (Zoom, Meet, Discord…), choisis le micro **« Beamer Microphone »**. Ensuite
  tu n'y touches plus : c'est Beamer qui décide quel vrai micro l'alimente.
- Le pilote « Beamer Microphone » est déjà installé. Sur un autre Mac, la page Audio propose « Installer »,
  avec le mot de passe administrateur.

**Sur le PC**
- VB-CABLE est déjà installé. Dans tes applis d'appel, choisis le micro **« CABLE Output »**. C'est l'équivalent
  de « Beamer Microphone ».
- Le pare-feu Windows a déjà été autorisé pour `beamer-audio.exe` (réseau privé).
- Option : dans la page Audio du PC, l'interrupteur **« Low-latency virtual microphone »** rend le micro
  plus réactif (environ 120 ms au lieu d'environ 170 ms mesurés). En contrepartie, aucune autre appli ne
  peut jouer dans CABLE Input pendant ce temps.

## 2. Où sont les réglages

Fenêtre de Beamer > page **Audio**, juste après « Crossing » (sur le PC : Ctrl+3). Elle contient :
- **Microphone** : le micro que tes applis entendent, sur toutes les machines. Le choisir une fois suffit.
- **Machines** : la carte des machines, comme celle des écrans. Clique une machine pour voir ses micros et
  ses sorties.
- **« Sound from … plays on »** : où sort le son de la machine sélectionnée. Avec « ici », rien ne change.
  Avec la sortie d'une autre machine, le son y est envoyé et coupé localement.
- Avertissements : écho, larsen, pilote manquant.

## 3. Petits tests (2 minutes chacun)

| # | Réglage dans la page Audio | Ce que tu dois constater |
|---|---|---|
| 1 | Microphone = Wave:3 (lebig) | Sur le Mac, QuickTime > Nouvel enregistrement audio > micro « Beamer Microphone » : ta voix arrive. Le son démarre dès que l'appli écoute. |
| 2 | Microphone = MacBook Air Microphone | Sur le PC, Enregistreur vocal ou Discord avec le micro « CABLE Output » : ta voix arrive. La première fois, macOS demande le Micro. |
| 3 | lebig > son joue sur MacBook Air Speakers | Le son du PC sort du Mac, et le G733 se tait. Remets « ici » : le G733 revient. |
| 4 | Mac > son joue sur le G733 (lebig) | Le son du Mac sort dans le G733, et le Mac se tait. La première fois, macOS demande l'Enregistrement audio du système. |
| 5 | Débranche puis rebranche un casque ou un micro USB | La route se met en attente, puis reprend seule sans rien re-régler. |
| 6 | Quitte Beamer pendant le test 3 ou 4 | Le son revient sur la machine d'origine en moins d'une seconde. |

Si quelque chose cloche, note l'heure : tout est dans les journaux (`~/Library/Logs/Beamer/Beamer.log`,
`%LOCALAPPDATA%\Beamer\Beamer.log`).

## 4. Ce qui marche (mesuré sur tes machines, en Wi-Fi)

| Chemin | Délai (un sens) |
|---|---|
| Micro du PC → appli du Mac | 41-56 ms |
| Son du PC → haut-parleurs du Mac (PC coupé) | 65 ms |
| Son du Mac → G733 (Mac coupé) | 70 ms |
| Micro du Mac → appli du PC via CABLE | 172 ms mesurés en mode normal, 120 ms en faible latence ; correctif déployé, pas remesuré |

- **Reprise après un incident :** Wi-Fi bloqué, programme audio tué, appareil débranché, Beamer relancé : tout reprend seul.
  Le son local revient en moins de 0,3 s.
- **Tenue dans la durée :** dérive d'horloge corrigée, aucun son perdu côté PC, mémoire stable sur 30 min.
- **Compatibilité :** asusrog (Beamer officiel) n'est pas touché, et aucune règle de pare-feu n'a été ajoutée par Beamer.

## 5. Ce qui ne marche pas encore, ou n'est pas testé

- **Larsen via Wave Link** : le retour micro de Wave Link renvoie ton Wave:3 dans le son du PC. Si ce son
  joue sur les haut-parleurs du Mac, ça peut siffler. L'anti-larsen le coupe, mais il se déclenche aussi
  parfois sur des voix fortes : le son du PC baisse alors de 3 à 19 s. Avec le casque sur le Mac, ou le
  retour Wave Link vers la sortie système coupé, ce problème disparaît.
- **Annulation d'écho** : le correctif de ce soir (elle injectait un peu du son du PC dans le micro) est
  testé, mais pas réécouté en vrai.
- **Jamais essayé en vrai** :
  - le vrai micro du Mac vers le PC (il attend l'autorisation) ;
  - un vrai branchement USB ou Bluetooth sur le PC ;
  - la mise en veille puis le réveil ;
  - Zoom et Discord eux-mêmes ;
  - l'écoute à l'oreille que le G733 se tait vraiment.
- **Changement de volume du G733 pendant un renvoi** : c'est désormais ton réglage qui gagne. La page
  n'affiche pas encore « le son joue aussi ici » si tu réactives le son toi-même.
- **Linux (asusrog)** : pas d'audio pour l'instant.
- **Langue** : les nouveaux textes de la page Audio sont en anglais, sans traduction.

## 6. La suite possible

1. **Tes retours de test** : c'est ce qui compte maintenant.
2. **Finir les deux points ouverts** : un anti-larsen moins sensible à la voix, et la réécoute de l'annulation d'écho.
3. **Préparer pour d'autres utilisateurs** :
   - installation du pilote Mac hors mode dev (mot de passe admin) ;
   - installateur Windows qui guide vers VB-CABLE ;
   - signature officielle et notarisation : elles dépendent du mainteneur de Beamer ;
   - proposer le travail en amont (kalkman-code/beamer).
4. **Plus tard** : Linux comme machine audio.
