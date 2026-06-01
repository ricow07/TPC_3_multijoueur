// protocol.h — Agar.io 3D (cube) — LAN multi-clients
#ifndef PROTOCOL_H
#define PROTOCOL_H

#define MAX_PLAYERS  16
#define MAX_FOOD     300
#define MAP_SIZE     2000.0f
#define SERVER_PORT  5000

#define DISCOVERY_ID  -1
#define SERVER_MAGIC  0x41474233   /* "AGB3" */

typedef struct {
    int   id;
    float dir_x, dir_y, dir_z;
} ClientInputPacket;

typedef struct {
    int magic;
    int assigned_id;
} DiscoveryReplyPacket;

typedef struct {
    int   id;
    float x, y, z;
    float radius;
} PlayerState;

typedef struct {
    float x, y, z;
    int   color;
} FoodState;

typedef struct {
    int         player_count;
    PlayerState players[MAX_PLAYERS];
    int         food_count;
    FoodState   food[MAX_FOOD];
} WorldStatePacket;

#endif
