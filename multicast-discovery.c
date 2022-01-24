#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <arpa/inet.h>

#include <net/if.h>
#include <netinet/in.h>

#include <unistd.h>

struct rawargs {
    int show_usage;
    char* multicast_groups;
    char* intervals;
    char* error;
};

struct rawargs* parse_args(int argc, char *argv[]) {
    char* argv0 = argv[0];
    int c = 0;
    struct rawargs* args = (struct rawargs*)malloc(sizeof(struct rawargs));

    while ((c = getopt(argc, argv, "hm:i:")) != -1) {
        switch (c) {
            case 'h':
                args->show_usage = 1;
                break;
            case 'm': {
                size_t len = strlen(optarg);
                args->multicast_groups = (char*)malloc(len * sizeof(char));
                strcpy(args->multicast_groups, optarg);
                break;
            }
            case 'i': {
                size_t len = strlen(optarg);
                args->intervals = (char*)malloc(len * sizeof(char));
                strcpy(args->intervals, optarg);
                break;
            }
            case '?':
                if (optopt == 'm' || optopt == 'i') sprintf(args->error, "Option `-%c` requires an argument.\n", optopt);
                else if (isprint(optopt)) sprintf(args->error, "Unknown option `-%c`.\n", optopt);
                else sprintf(args->error, "Unknown option character `\\x%x`.\n", optopt);
                return args;
            default:
                abort();
        }
    }

    return args;
}

void print_usage(const char* argv0) {
    printf("Usage:\n");
    printf("  `%s -h` - show help and exit;\n", argv0);
    printf("  `%s -m <multicast-groups-addresses> -i <hello-interval,dead-interval>` - start discovery of selected groups.\n", argv0);
}

char** split(const char* src, const char* delim) {
    size_t entries;
    char* copy = (char*)malloc(strlen(src) + 1);
    char* copy_ptr = copy;
    strcpy(copy, src);
    for (entries=0; copy[entries]; copy[entries]==delim[0] ? entries++ : *copy++);
    copy -= strlen(src) - entries;
    entries += 1;

    char** result = (char**)malloc((entries + 1) * sizeof(char*));
    memset(result + entries * sizeof(char*), 0, sizeof(char*));

    size_t i = 0;
    char* token = strtok(copy, delim);
    while (token) {
        result[i] = (char*)malloc(strlen(token) + 1);
        strcpy(result[i], token);
        i++;
        token = strtok(NULL, delim);
    }

    free(copy_ptr);
    return result;
}

int get_address_family(const char* addr) {
    char buf[16];
    if (inet_pton(AF_INET, addr, buf)) return AF_INET;
    if (inet_pton(AF_INET6, addr, buf)) return AF_INET6;
    return -1;
}

int get_server_sock(int addr_family) {
    int fd = socket(addr_family, SOCK_DGRAM, 0);
    if (fd == -1) {
        return -1;
    }

    if (addr_family == AF_INET6) {
        uint yes = 1;
        if (setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &yes, sizeof(yes)) < 0) {
            perror("setsockopt v6only");
            return -2;
        }
    }

    struct sockaddr* sockaddr;
    socklen_t sockaddr_len;
    switch (addr_family) {
        case AF_INET: {
            sockaddr_len = sizeof(struct sockaddr_in);
            struct sockaddr_in* addr_v4 = (struct sockaddr_in*)malloc(sockaddr_len);
            addr_v4->sin_addr.s_addr = INADDR_ANY;
            addr_v4->sin_family = AF_INET;
            addr_v4->sin_port = htons(4136);
            sockaddr = (struct sockaddr*)addr_v4;
            break;
        }
        case AF_INET6: {
            sockaddr_len = sizeof(struct sockaddr_in6);
            struct sockaddr_in6* addr_v6 = (struct sockaddr_in6*)malloc(sockaddr_len);
            addr_v6->sin6_addr = in6addr_any;
            addr_v6->sin6_family = AF_INET6;
            addr_v6->sin6_port = htons(4136);
            sockaddr = (struct sockaddr*)addr_v6;
            break;
        }
        default:
            return -3;
    }

    if (bind(fd, sockaddr, sockaddr_len) < 0) {
        perror("bind");
        return -4;
    }

    return fd;
}

int subscribe_server_sock(int fd, const char* addr) {
    int addr_family = get_address_family(addr);
    if (addr_family == -1) {
        return -1;
    }

    switch (addr_family) {
        case AF_INET: {
            struct ip_mreq mreq;
            inet_pton(AF_INET, addr, &mreq.imr_multiaddr);
            mreq.imr_interface.s_addr = htonl(INADDR_ANY); // FIXME!!!!!!
            if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
                return -2;
            }
            break;
        }
        case AF_INET6: {
            struct ipv6_mreq mreq;
            inet_pton(AF_INET6, addr, &mreq.ipv6mr_multiaddr);
            mreq.ipv6mr_interface = 0; // FIXME!!!!!!
            if (setsockopt(fd, IPPROTO_IPV6, IPV6_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
                return -3;
            }
            break;
        }
        default:
            return -4;
    }

    return 0;
}

struct intervals_t {
    int hello;
    int dead;
};

int main(int argc, char *argv[]) {
    char* argv0 = argv[0];
    struct rawargs* args = parse_args(argc, argv);

    if (args->error) {
        print_usage(argv0);
        return 1;
    }
    if (args->show_usage) {
        print_usage(argv0);
        return 0;
    }

    int v4_server_sock = -1;
    int v6_server_sock = -1;
    {
        char** groups = split(args->multicast_groups, ",");
        size_t i = 0;
        char* group = NULL;
        while ((group = groups[i++]) != NULL) {
            int addr_family = get_address_family(group);
            int *sock = NULL;
            switch (addr_family) {
                case AF_INET:
                    sock = &v4_server_sock;
                    break;
                case AF_INET6:
                    sock = &v6_server_sock;
                    break;
                default:
                    fprintf(stderr, "Wrong multicast group: %s\n", group);
                    return 2;
            }
            if (*sock < 0) {
                *sock = get_server_sock(addr_family);
            }
            if (*sock < 0) {
                fprintf(stderr, "Cannot create listening socket for %s\n", group);
                return 2;
            }
            if (subscribe_server_sock(*sock, group) < 0) {
                fprintf(stderr, "Cannot subscribe listening socket for group %s\n", group);
                return 3;
            }
            printf("Listening for group %s\n", group);
        }
    }

    struct intervals_t intervals;
    {
        char** ints = split(args->intervals, ",");
        size_t i = 0;
        char* interval = NULL;
        while ((interval = ints[i++]) != NULL) {
            switch (i) {
                case 1:
                    intervals.hello = atoi(interval);
                    break;
                case 2:
                    intervals.dead = atoi(interval);
                    break;
                default:
                    fprintf(stderr, "Unexpected interval met: %s\n", interval);
                    return 4;
            }
        }
    }
    printf("Hello interval is %d\n", intervals.hello);
    printf("Dead interval is %d\n", intervals.dead);

    getchar();
    return 0;
}
