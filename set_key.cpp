#include "mavlink.h"

#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <arpa/inet.h>

#include "tdb.h"
#include "sha256.h"

int main(int argc, char *argv[])
{
    if (argc < 3) {
        printf("Usage: set_key KEY_ID PASSPHRASE\n");
        exit(1);
    }

    int key_id = atoi(argv[1]);
    const char *passphrase = argv[2];

    sha256_ctx ctx;

    MAVLinkUDP::SigningKey key;
    key.timestamp = 0;
    key.magic = KEY_MAGIC;

    sha256_init(&ctx);
    sha256_update(&ctx, passphrase, strlen(passphrase));
    sha256_final_32bytes(&ctx, key.secret_key);

    if (!MAVLinkUDP::save_key(key_id, key)) {
        ::printf("Failed to save key for %d\n", key_id);
        exit(1);
    }
    ::printf("saved key for %d\n", key_id);
    return 0;
}
