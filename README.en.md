# Agar.io 3D (Cube) - LAN Multiplayer

**Collaborative School Project**
This project was carried out as a group during our computer science studies at Ynov Campus.

## Description
We developed a 3D version (in the form of cubes) inspired by the famous game Agar.io. It is a multiplayer game running on a local area network (LAN). The project relies on a robust client/server architecture developed entirely in C language. Players can connect to the game server, move freely in a large 3D space, consume food to make their cube grow, and interact with other participants. A real-time chat system is also integrated to communicate during the game.

## Tech Stack
- **Language**: C
- **Architecture**: Client / Server
- **Network**: Sockets (LAN), creation of a custom communication protocol (`protocol.h`)

## Installation and Launch
The repository already contains compiled binaries for Windows, ready to use.

### Launching the server
1. Open this folder in Windows Explorer.
2. Double-click the `server.exe` file.
3. The server starts listening on the local network and can host up to 16 players simultaneously.

### Launching the client
1. Make sure the server is running.
2. Double-click on `client.exe`.
3. The game opens and automatically connects to the detected server. Have fun!

## Project Structure
```
TPC_3_multijoueur/
├── server_main.c    # Server-side logic and network management
├── client_main.c    # Client-side render engine, inputs, and network
├── protocol.h       # Packet definition (PlayerState, FoodState, etc.)
├── server.exe       # Compiled server executable
├── client.exe       # Compiled client executable (player)
├── server_test.exe  # Test binary
├── client_test.exe  # Test binary
└── README.md        # Documentation
```

### 👥 Contributors
- Ynov Campus Students Group.
