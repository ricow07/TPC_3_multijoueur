# Agar.io 3D (Cube) - LAN Multi-clients

**Projet scolaire collaboratif**
Ce projet a été réalisé en groupe dans le cadre de nos études en informatique à Ynov Campus.

## Description
Nous avons développé une version 3D (sous forme de cubes) inspirée du célèbre jeu Agar.io. Il s'agit d'un jeu multijoueur fonctionnant en réseau local (LAN). Le projet repose sur une architecture client/serveur robuste développée entièrement en langage C. Les joueurs peuvent se connecter au serveur de la partie, se déplacer librement dans un grand espace 3D, consommer de la nourriture pour faire grandir leur cube, et interagir avec les autres participants. Un système de chat en temps réel est également intégré pour communiquer durant la partie.

## Stack Technique
- **Langage** : C
- **Architecture** : Client / Serveur
- **Réseau** : Sockets (LAN), création d'un protocole de communication sur-mesure (`protocol.h`)

## Installation et Lancement
Le dépôt contient déjà les binaires compilés pour Windows, prêts à l'emploi.

### Lancement du serveur
1. Ouvrez ce dossier dans l'explorateur Windows.
2. Double-cliquez sur le fichier `server.exe`.
3. Le serveur se met en écoute sur le réseau local et peut accueillir jusqu'à 16 joueurs simultanément.

### Lancement du client
1. Assurez-vous que le serveur est bien lancé.
2. Double-cliquez sur `client.exe`.
3. Le jeu s'ouvre et se connecte automatiquement au serveur détecté. Amusez-vous bien !

## Arborescence du Projet
```
TPC_3_multijoueur/
├── server_main.c    # Logique et gestion réseau côté serveur
├── client_main.c    # Moteur de rendu, inputs et réseau côté client
├── protocol.h       # Définition des paquets (PlayerState, FoodState, etc.)
├── server.exe       # Exécutable compilé du serveur
├── client.exe       # Exécutable compilé du client (joueur)
├── server_test.exe  # Binaire de test
├── client_test.exe  # Binaire de test
└── README.md        # Documentation
```

### 👥 Contributeurs
- Groupe d'étudiants Ynov Campus.
