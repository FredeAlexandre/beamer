# Tester la caméra de Beamer (caméra du Mac → PC)

But : la caméra du MacBook Air apparaît sur lebig comme une webcam branchée, sous le nom **« Beamer Camera »**,
dans toutes les applis (Caméra, Teams, Discord, navigateur). Les textes de Beamer sont en anglais, sans
traduction pour l'instant. Compte environ 30 min, plus 15 min avec l'opérateur pour les mesures (étape 5).

## Résultats des tests automatiques

(rempli par l'opérateur après les runs E2E du 2026-11-12)

Version installée sur les deux machines : …

## 0. Avant de commencer

- Les deux machines sont allumées, Beamer tourne des deux côtés, et ta session est ouverte sur lebig.
- Les réglages sont dans Beamer > page **« Audio & Camera »** (l'ancienne page Audio, toujours Ctrl+3 sur le
  PC). Le module Caméra s'appelle **« Camera your apps see »**. C'est là qu'on choisit quelle caméra les applis
  du PC voient.
- Si quelque chose cloche, **note l'heure** et ce que tu vois (une photo d'écran suffit). Tout est dans les
  journaux (voir la fin).

## 1. Installer Beamer Camera sur le PC (H6, 2 min)

- [ ] Sur lebig, page Audio & Camera : la ligne « This PC » dit « Not installed ». Clique **« Install Beamer
  Camera… »**.
- [ ] Windows affiche une demande UAC pour `beamer-audio.exe` : clique **Oui**.
  Attendu : en ~5 s, la ligne passe à « Installed ».
- [ ] **Pare-feu Windows** : note si une fenêtre du pare-feu apparaît pour `beamer-audio.exe`, à n'importe quel
  moment de ce guide, et ce que tu as cliqué. Elle ne devrait pas apparaître chez toi, puisque tu l'as déjà
  autorisé pour l'audio. Un autre utilisateur la verrait une fois.
- [ ] Option (une fois) : refais l'installation en cliquant **Non** à l'UAC. La page doit dire « Installation
  was cancelled. » et rien ne doit changer.

À savoir : une **mise à jour** de Beamer Camera passe par « Update Beamer Camera… » (même UAC). Elle ne
demande **pas de redémarrage**. Si une appli utilise la caméra, la page dit « Close apps that use the camera
to finish updating Beamer Camera. » : ferme-les, et la nouvelle version est prise ~30 s après la fermeture de la
dernière appli.

## 2. Premier usage et autorisation caméra du Mac (H1, 3 min)

Règle de confidentialité : macOS ne doit poser la question de la caméra **qu'après une action sur le Mac**,
jamais à cause d'un clic sur le PC.

- [ ] Sur le **PC**, dans « Camera your apps see », choisis la caméra du MacBook Air.
  Attendu : **aucune** demande sur le Mac. À la place, le menu Beamer du Mac propose « Allow Camera Access… »
  et sa page affiche « Beamer needs camera access to share this Mac's camera. »
- [ ] Sur le **Mac**, clique **« Allow Camera Access… »** (menu ou page).
  Attendu : la demande de macOS nomme **Beamer**, avec le texte « Beamer sends this Mac's camera to another
  computer while an app there uses Beamer Camera… ». Clique **Autoriser**.
- [ ] Réglages Système > Confidentialité et sécurité > Caméra : Beamer est listé et activé.
- [ ] Sur le PC, ouvre l'appli **Caméra** et choisis « Beamer Camera ». Attendu : ton image arrive en environ
  1 s, et le voyant vert du Mac s'allume. La page du Mac affiche « <appli> on lebig is using it. », avec la taille,
  les fps et le délai en dessous.

Si la demande de macOS apparaît dès le clic sur le PC, ou qu'elle nomme autre chose que Beamer, note-le :
c'est un défaut important.

## 3. Les applis du PC (H2, H3, 10 min)

Teams, Discord et l'appli Caméra demandent l'accès à la caméra à Windows au premier usage : accepte.
Selon l'appli, le nom peut apparaître comme « Beamer Camera (Caméra virtuelle Windows) ».

| Appli | À vérifier |
|---|---|
| Caméra (Windows) | « Beamer Camera » listée, ton image |
| Teams | Paramètres > Appareils : « Beamer Camera » listée, aperçu correct |
| Discord | Paramètres > Voix et vidéo : « Beamer Camera », test vidéo |
| Navigateur (Edge ou Chrome) | un site de test de webcam : « Beamer Camera » proposée, image |

- [ ] Chaque ligne du tableau. Note celles où la caméra manque ou l'image est noire.
- [ ] **Image d'attente** : sur le PC, mets « Camera your apps see » sur « Off », puis ouvre l'appli Caméra.
  Attendu : jamais d'écran noir, mais une image Beamer « Beamer Camera is off » / « Choose a camera in Beamer ».
  Remets la caméra du Mac : l'image revient.
- [ ] **Plusieurs applis à la fois** : ouvre la caméra dans l'appli Caméra, puis dans Teams. Note ce que fait la
  deuxième. Par défaut, Windows ne laisse qu'une appli à la fois.
- [ ] Sur la page du PC, la phrase « One app at a time can use Beamer Camera unless Windows allows several… »
  a un lien **« Camera settings »**. Il doit ouvrir Paramètres > Bluetooth et appareils > Caméras. Ouvre
  « Beamer Camera », active le réglage qui autorise plusieurs applis, puis refais le test. Note ce que tu vois.

## 4. Confidentialité (5 min)

- [ ] **Extinction** : appli Caméra du PC ouverte, voyant vert du Mac allumé. Ferme l'appli.
  Attendu : le voyant s'éteint **3 à 5 s** après. Avec plusieurs applis ouvertes, il reste allumé jusqu'à la
  fermeture de la dernière.
- [ ] **Interrupteur du Mac** : pendant que le PC affiche l'image, choisis **« Stop Sharing the Camera »** dans
  le menu Beamer du Mac. Attendu : le voyant s'éteint tout de suite. L'appli du PC affiche « Camera turned off
  on MacBook Air ». La page du PC dit « Turned off on MacBook Air. » sans bouton pour passer outre. Essaie de
  rechoisir la caméra du Mac depuis le PC : elle doit rester éteinte. « Share the Camera » sur le Mac la rallume.
- [ ] **Coupure réseau** : pendant un appel, coupe le Wi-Fi du Mac ~10 s, puis rallume-le. Attendu : le PC garde
  la dernière image environ 2 s, puis affiche « Reconnecting to MacBook Air ». Note si l'image revient seule et
  en combien de temps.
- [ ] Si une ligne « Reduced quality: the network is slow. » ou « through the Beamer link » apparaît : c'est le
  mode de secours par le lien Beamer (moins net, 15 fps). Note quand.

## 5. Qualité, synchro, délai (H5, 15 min, avec l'opérateur pour les mesures)

- [ ] **720p / 1080p** : 720p par défaut. Passe Quality sur **1080p** sur la page, appli Caméra ouverte. Attendu :
  la ligne de détail indique 1920x1080 et l'image reste nette. Remets 720p.
- [ ] **Faible lumière** : éteins la lumière. Note la fluidité et les fps affichés sur la page du Mac (15 fps
  possibles dans le noir).
- [ ] **Effets vidéo** : caméra active, ouvre le Centre de contrôle du Mac > Effets vidéo (Portrait, Éclairage
  de studio, Cadre centré). Note si l'effet se voit sur le PC. Note aussi si un geste de la main déclenche une
  animation : ces réactions devraient être désactivées.
- [ ] **Synchro son/image (clap)**, avec l'opérateur (`cam_lipsync_real`) : micro du Mac envoyé vers le PC, tape
  dans tes mains devant la caméra. Beamer retarde l'image pour qu'elle colle au micro (150 ms au plus). Note si
  le son et l'image du clap te semblent décalés.
- [ ] **Délai**, avec l'opérateur (`run.py real` : `cam_start_real` puis `cam_latency_real`, ~10 min) : un
  chronomètre de téléphone devant la caméra du Mac, l'appli Caméra ouverte sur le PC. Prends en photo le
  téléphone et l'écran du PC ensemble. L'écart entre les deux chronos donne le délai (~85-130 ms attendus).

## 6. Redémarrage et déconnexion du PC (H4, H9, 5 min)

- [ ] Déconnecte ta session Windows, puis reconnecte-toi. Ouvre l'appli Caméra : « Beamer Camera » est toujours
  listée et l'image arrive.
- [ ] Redémarre lebig, puis ouvre ta session. Même vérification. Préviens l'opérateur une fois connecté : il
  fait alors une mesure de mise à jour de la caméra.

## 7. Désinstallation (H7, optionnel, à faire en dernier)

Cette étape **retire Beamer du PC** : l'opérateur le réinstalle après. Ne la fais qu'avec son accord.

- [ ] Paramètres > Applications > Applications installées > Beamer > Désinstaller, et accepte l'UAC.
- [ ] Appli Caméra, Teams, navigateur : « Beamer Camera » n'est plus listée. Note si elle reste quelque part.

## Si quelque chose ne va pas

- **Journaux** : Mac `~/Library/Logs/Beamer/Beamer.log`, PC `%LOCALAPPDATA%\Beamer\Beamer.log`. Donne l'heure
  du problème à l'opérateur.
- **Retirer l'autorisation caméra du Mac** : Réglages Système > Confidentialité et sécurité > Caméra > désactive
  Beamer. La page du Mac affiche alors « This Mac does not allow Beamer to use its camera. » avec un bouton
  « Open Privacy Settings ».
- **Caméra coupée vite fait** : « Stop Sharing the Camera » dans le menu Beamer du Mac. Ce réglage gagne
  toujours sur le PC.
- **Image figée, noire ou absente** : photographie l'écran du PC et note le texte de l'image d'attente
  (« Waiting for… », « Reconnecting to… », « Beamer Camera stopped »…). Il dit où la chaîne s'arrête.
