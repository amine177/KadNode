
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <signal.h>
#include <limits.h>
#include <errno.h>

#include "main.h"
#include "log.h"
#include "utils.h"
#include "conf.h"
#include "values.h"
#include "peerfile.h"
#include "kad.h"
#ifdef TLS
#include "ext-tls.h"
#include "ext-tls-server.h"
#endif
#ifdef BOB
#include "ext-bob.h"
#endif
#ifdef FWD
#include "ext-fwd.h"
#endif
#ifdef __CYGWIN__
#include "windows.h"
#endif

// Global object variables
struct gconf_t *gconf = NULL;

static int g_argc = NULL;
static char **g_argv = NULL;


const char *kadnode_version_str = "KadNode v"MAIN_VERSION" ("
#ifdef LPD
" lpd"
#endif
#ifdef BOB
" bob"
#endif
#ifdef CMD
" cmd"
#endif
#ifdef NSS
" nss"
#endif
#ifdef DEBUG
" debug"
#endif
#ifdef DNS
" dns"
#endif
#ifdef FWD_NATPMP
" natpmp"
#endif
#ifdef FWD_UPNP
" upnp"
#endif
#ifdef TLS
" tls"
#endif
#ifdef WEB
" web"
#endif
" )";

const char *kadnode_usage_str = "KadNode - A P2P name resolution daemon.\n"
"A Wrapper for the Kademlia implementation of a Distributed Hash Table (DHT)\n"
"with several optional interfaces (use --version).\n"
"\n"
"Usage: kadnode [OPTIONS]*\n"
"\n"
" --value-id <id>[:<port>]	Add a value/domain to be announced every 30 minutes.\n"
"				This option may occur multiple times.\n\n"
" --peerfile <file>		Import/Export peers from and to a file.\n\n"
" --peer <addr>			Add a static peer address.\n"
"				This option may occur multiple times.\n\n"
" --user <user>			Change the UUID after start.\n\n"
" --port	<port>			Bind DHT to this port.\n"
"				Default: "DHT_PORT"\n\n"
" --addr	<addr>			Bind DHT to this address.\n"
"				Default: "DHT_ADDR4" / "DHT_ADDR6"\n\n"
" --config <file>		Provide a configuration file with one command line\n"
"				option on each line. Comments start after '#'.\n\n"
" --ifname <interface>		Bind to this interface.\n"
"				Default: <any>\n\n"
" --daemon			Run the node in background.\n\n"
" --verbosity <level>		Verbosity level: quiet, verbose or debug.\n"
"				Default: verbose\n\n"
" --pidfile <file>		Write process pid to a file.\n\n"
" --mode <ipv4|ipv6>		Enable IPv4 or IPv6 mode for the DHT.\n"
"				Default: ipv4\n\n"
" --query-tld <domain>		Top level domain to be handled by KadNode.\n"
"				Default: "QUERY_TLD_DEFAULT"\n\n"
#ifdef LPD
" --lpd-addr <addr>		Set multicast address for Local Peer Discovery.\n"
"				Default: "LPD_ADDR4" / "LPD_ADDR6"\n\n"
" --lpd-disable			Disable multicast to discover local peers.\n\n"
#endif
#ifdef BOB
" --bob-gen-pair		Generate a new public/secret key pair and exit.\n\n"
" --bob-add-skey <key>	Add a secret key. The derived public key will be announced.\n"
"				The secret key will be used to prove that you have it.\n"
#endif
#ifdef CMD
" --cmd-disable-stdin		Disable the local control interface.\n\n"
" --cmd-port <port>		Bind the remote control interface to this local port.\n"
"				Default: "CMD_PORT"\n\n"
#endif
#ifdef DNS
" --dns-port <port>		Bind the DNS server interface to this local port.\n"
"				Default: "DNS_PORT"\n\n"
" --dns-server <ip_addr>	IP address of an external DNS server. Enables DNS proxy mode.\n"
"				Default: none\n\n"
#endif
#ifdef NSS
" --nss-port <port>		Bind the Network Service Switch to this local port.\n"
"				Default: "NSS_PORT"\n\n"
#endif
#ifdef WEB
" --web-port <port>		Bind the web server to this local port.\n"
"				Default: "WEB_PORT"\n\n"
#endif
#ifdef FWD
" --fwd-disable			Disable UPnP/NAT-PMP to forward router ports.\n\n"
#endif
#ifdef TLS
"--tls-client-entry		Path to file or folder of CA certificates for TLS client.\n\n"
"--tls-server-entry		Comma separated triples of domain, certificate and key for TLS server.\n"
"				Example: kanode.p2p,kadnode.crt,kadnode.key\n\n"
#endif
#ifdef __CYGWIN__
" --service-start		Start, install and remove KadNode as Windows service.\n"
" --service-install		KadNode will be started/shut down along with Windows\n"
" --service-remove		or on request by using the Service Control Manager.\n\n"
#endif
" -h, --help			Print this help.\n\n"
" -v, --version			Print program version.\n";


// Parse a <id>[:<port>] value
void conf_apply_value( const char val[] ) {
	int port;
	int rc;
	char *p;

#ifdef FWD
	int is_random_port = 0;
#endif

	// Find <id>:<port> delimiter
	p = strchr( val, ':' );

	if( p ) {
		*p = '\0';
		port = port_parse( p + 1, -1 );
	} else {
		// A valid port will be choosen inside kad_announce()
		port = 0;
#ifdef FWD
		is_random_port = 1;
#endif
	}

	rc = kad_announce( val, port, LONG_MAX );
	if( rc < 0 ) {
		log_err( "CFG: Invalid port for value annoucement: %d", port );
		exit( 1 );
	} else {
#ifdef FWD
		if( !is_random_port ) {
			fwd_add( port, LONG_MAX );
		}
#endif
	}
}

void conf_init( void ) {
	gconf = (struct gconf_t *) calloc( 1, sizeof(struct gconf_t) );

	gconf->is_running = 1;

#ifdef DEBUG
	gconf->verbosity = VERBOSITY_DEBUG;
#else
	gconf->verbosity = VERBOSITY_VERBOSE;
#endif
}

// Set default if setting was not set and validate settings
void conf_check( void ) {
	uint8_t node_id[SHA1_BIN_LENGTH];
	char hexbuf[SHA1_HEX_LENGTH+1];

	if( gconf->af == 0 ) {
		gconf->af = AF_INET;
	}

	if( gconf->query_tld == NULL ) {
		gconf->query_tld = strdup( QUERY_TLD_DEFAULT );
	}

	if( gconf->node_id_str == NULL ) {
		bytes_random( node_id, SHA1_BIN_LENGTH );
		str_id( node_id, hexbuf );
		gconf->node_id_str = strdup( hexbuf );
	}

	if( gconf->dht_port == NULL ) {
		gconf->dht_port = strdup( DHT_PORT );
	}

	if( gconf->dht_addr == NULL ) {
		gconf->dht_addr = strdup(
			(gconf->af == AF_INET) ? DHT_ADDR4 : DHT_ADDR6
		);
	}

#ifdef CMD
	if( gconf->cmd_port == NULL )  {
		gconf->cmd_port = strdup( CMD_PORT );
	}
#endif

#ifdef DNS
	if( gconf->dns_port == NULL ) {
		gconf->dns_port = strdup( DNS_PORT );
	}

	if( gconf->dns_server ) {
		if( addr_parse( &gconf->dns_server_addr, gconf->dns_server, "53", AF_UNSPEC ) != 0 ) {
			log_err( "CFG: Failed to parse IP address '%s'.", gconf->dns_server );
			exit( 1 );
		}
	}
#endif

#ifdef NSS
	if( gconf->nss_port == NULL ) {
		gconf->nss_port = strdup( NSS_PORT );
	}
#endif

#ifdef WEB
	if( gconf->web_port == NULL ) {
		gconf->web_port = strdup( WEB_PORT );
	}
#endif

	if( port_parse( gconf->dht_port, -1 ) < 1 ) {
		log_err( "CFG: Invalid DHT port '%s'.", gconf->dht_port );
		exit( 1 );
	}

#ifdef CMD
	if( port_parse( gconf->cmd_port, -1 ) < 0 ) {
		log_err( "CFG: Invalid CMD port '%s'.", gconf->cmd_port );
		exit( 1 );
	}
#endif

#ifdef DNS
	if( port_parse( gconf->dns_port, -1 ) < 0 ) {
		log_err( "CFG: Invalid DNS port '%s'.", gconf->dns_port );
		exit( 1 );
	}
#endif

#ifdef NSS
	if( port_parse( gconf->nss_port, -1 ) < 0 ) {
		log_err( "CFG: Invalid NSS port '%s'.", gconf->nss_port );
		exit( 1 );
	}
#endif

#ifdef WEB
	if( port_parse( gconf->web_port, -1 ) < 0 ) {
		log_err( "CFG: Invalid WEB port '%s'.", gconf->web_port );
		exit( 1 );
	}
#endif

#ifdef LPD
	IP lpd_addr;
	uint8_t octet;

	if( gconf->lpd_addr == NULL ) {
		// Set default multicast address string
		if( gconf->af == AF_INET ) {
			gconf->lpd_addr = strdup( LPD_ADDR4 );
		} else {
			gconf->lpd_addr = strdup( LPD_ADDR6 );
		}
	}

	// Parse multicast address string
	if( addr_parse( &lpd_addr, gconf->lpd_addr, LPD_PORT, gconf->af ) != 0 ) {
		log_err( "CFG: Failed to parse IP address for '%s'.", gconf->lpd_addr );
		exit( 1 );
	}

	// Verifiy multicast address
	if( gconf->af == AF_INET ) {
		octet = ((uint8_t *) &((IP4 *)&lpd_addr)->sin_addr)[0];
		if( octet != 224 && octet != 239 ) {
			log_err( "CFG: Multicast address expected: %s", str_addr( &lpd_addr ) );
			exit( 1 );
		}
	} else {
		octet = ((uint8_t *) &((IP6 *)&lpd_addr)->sin6_addr)[0];
		if( octet != 0xFF ) {
			log_err( "CFG: Multicast address expected: %s", str_addr( &lpd_addr ) );
			exit( 1 );
		}
	}
#endif

	// Store startup time
	gettimeofday( &gconf->time_now, NULL );
	gconf->startup_time = time_now_sec();
}

void conf_info( void ) {
	log_info( "Starting %s", kadnode_version_str );
	log_info( "Node ID: %s", gconf->node_id_str );
	log_info( "IP Mode: %s", (gconf->af == AF_INET) ? "IPv4" : "IPv6");

	if( gconf->is_daemon ) {
		log_info( "Run Mode: Daemon" );
	} else {
		log_info( "Run Mode: Foreground" );
	}

	if( gconf->configfile ) {
		log_info( "Configuration File: '%s'", gconf->configfile );
	}

	switch( gconf->verbosity ) {
		case VERBOSITY_QUIET:
			log_info( "Verbosity: quiet" );
			break;
		case VERBOSITY_VERBOSE:
			log_info( "Verbosity: verbose" );
			break;
		case VERBOSITY_DEBUG:
			log_info( "Verbosity: debug" );
			break;
		default:
			log_err( "Invalid verbosity level." );
			exit( 1 );
	}

	log_info( "Query TLD: %s", gconf->query_tld );
	log_info( "Peer File: %s", gconf->peerfile ? gconf->peerfile : "None" );
#ifdef LPD
	log_info( "LPD Address: %s", (gconf->lpd_disable == 0) ? gconf->lpd_addr : "Disabled" );
#endif
#ifdef DNS
	if (gconf->dns_server) {
		log_info( "Forward foreign DNS requests to %s", gconf->dns_server );
	}
#endif
}

void conf_free( void ) {
	free( gconf->query_tld );
	free( gconf->node_id_str );
	free( gconf->user );
	free( gconf->pidfile );
	free( gconf->peerfile );
	free( gconf->dht_port );
	free( gconf->dht_ifname );
	free( gconf->configfile );

#ifdef LPD
	free( gconf->lpd_addr );
#endif
#ifdef CMD
	free( gconf->cmd_port );
#endif
#ifdef DNS
	free( gconf->dns_port );
	free( gconf->dns_server );
#endif
#ifdef NSS
	free( gconf->nss_port );
#endif
#ifdef WEB
	free( gconf->web_port );
#endif

	free( gconf );
}

enum OpCode {
	oQueryTld,
	oPidFile,
	oPeerFile,
	oPeer,
	oVerbosity,
	oCmdDisableStdin,
	oCmdPort,
	oDnsPort,
	oDnsServer,
	oNssPort,
	oTlsClientEntry,
	oTlsServerEntry,
	oWebPort,
	oConfig,
	oMode,
	oPort,
	oAddr,
	oLpdAddr,
	oLpdDisable,
	oFwdDisable,
	oServiceInstall,
	oServiceRemove,
	oServiceStart,
	oBobGenKeys,
	oBobAddSkey,
	oValueId,
	oIfname,
	oUser,
	oDaemon,
	oHelp,
	oVersion,
	oUnknown
};

static const struct {
	const char *name;
	enum OpCode code;
} options[] = {
	{"--query-tld", oQueryTld},
	{"--pidfile", oPidFile},
	{"--peerfile", oPeerFile},
	{"--peer", oPeer},
	{"--verbosity", oVerbosity},
#ifdef CMD
	{"--cmd-disable-stdin", oCmdDisableStdin},
	{"--cmd-port", oCmdPort},
#endif
#ifdef DNS
	{"--dns-port", oDnsPort},
	{"--dns-server", oDnsServer},
#endif
#ifdef NSS
	{"--nss-port", oNssPort},
#endif
#ifdef TLS
	{"--tls-client-entry", oTlsClientEntry},
	{"--tls-server-entry", oTlsServerEntry},
#endif
#ifdef WEB
	{"--web-port", oWebPort},
#endif
	{"--config", oConfig},
	{"--mode", oMode},
	{"--port", oPort},
	{"--addr", oAddr},
#ifdef LPD
	{"--lpd-addr", oLpdAddr},
	{"--lpd-disable", oLpdDisable},
#endif
#ifdef FWD
	{"--fwd-disable", oFwdDisable},
#endif
#ifdef __CYGWIN__
	{"--service-install", oServiceInstall},
	{"--service-remove", oServiceRemove},
	{"--service-start", oServiceStart},
#endif
#ifdef BOB
	{"--bob-gen-keys", oBobGenKeys},
	{"--bob-add-skey", oBobAddSkey},
#endif
	{"--value-id", oValueId},
	{"--ifname", oIfname},
	{"--user", oUser},
	{"--daemon", oDaemon},
	{"-h", oHelp},
	{"--help", oHelp},
	{"-v", oVersion},
	{"--version", oVersion},
};

unsigned findCode(const char name[]) {
	int i;

	for( i = 0; i < N_ELEMS(options); i++) {
		if( strcmp( name, options[i].name ) == 0 ) {
			return options[i].code;
		}
	}

	return oUnknown;
}

void conf_arg_expected( const char opt[] ) {
	log_err( "CFG: Argument expected for option: %s", opt );
	exit( 1 );
}

void conf_no_arg_expected( const char opt[] ) {
	log_err( "CFG: No argument expected for option: %s", opt );
	exit( 1 );
}

void conf_duplicate_option( const char opt[] ) {
	log_err( "CFG: Option was already set: %s", opt );
	exit( 1 );
}

// Set a string once - error when already set
void conf_str( const char opt[], char *dst[], const char src[] ) {
	if( src == NULL ) {
		conf_arg_expected( opt );
		return;
	}

	if( *dst != NULL ) {
		conf_duplicate_option( opt );
		return;
	}

	*dst = strdup( src );
}

// Add SNI entry for the TLS server
void tls_add_server_entry( const char opt[], const char val[] ) {
	char name[128];
	char crt_file[128];
	char key_file[128];

	if( sscanf( val, "%127[^,],%127[^,],%127[^,]", name, crt_file, key_file ) == 3 ) {
		tls_add_sni_entry( name, crt_file, key_file );
	} else {
		log_err( "CFG: Invalid option format: %s", val );
		exit(1);
	}
}

void conf_handle_option( const char opt[], const char val[] ) {

	switch( findCode(opt) ) {
		case oQueryTld:
			conf_str( opt, &gconf->query_tld, val );
			break;
		case oPidFile:
			conf_str( opt, &gconf->pidfile, val );
			break;
		case oPeerFile:
			conf_str( opt, &gconf->peerfile, val );
			break;
		case oPeer:
			if( val == NULL ) {
				conf_arg_expected( opt );
				break;
			}
			peerfile_add_peer( val );
			break;
		case oVerbosity:
			if( val == NULL ) {
				conf_arg_expected( opt );
				break;
			} else if( match( val, "quiet" ) ) {
				gconf->verbosity = VERBOSITY_QUIET;
			} else if( match( val, "verbose" ) ) {
				gconf->verbosity = VERBOSITY_VERBOSE;
			} else if( match( val, "debug" ) ) {
				gconf->verbosity = VERBOSITY_DEBUG;
			} else {
				log_err( "CFG: Invalid argument for %s.", opt );
				exit( 1 );
			}
			break;
#ifdef CMD
		case oCmdDisableStdin:
			if( val != NULL ) {
				conf_no_arg_expected( opt );
			} else {
				gconf->cmd_disable_stdin = 1;
			}
			break;
		case oCmdPort:
			conf_str( opt, &gconf->cmd_port, val );
			break;
#endif
#ifdef DNS
		case oDnsPort:
			conf_str( opt, &gconf->dns_port, val );
			break;
		case oDnsServer:
			conf_str( opt, &gconf->dns_server, val );
			break;
#endif
#ifdef NSS
		case oNssPort:
			conf_str( opt, &gconf->nss_port, val );
			break;
#endif
#ifdef TLS
		case oTlsClientEntry:
			// Add Certificate Authority (CA) entries for the TLS client
			if( tls_add_ca_entry( val ) != 0 ) {
				exit(1);
			}
			break;
		case oTlsServerEntry:
		{
			// Add SNI entries for the TLS server (e.g. foo.p2p,my.cert,my.key)
			char name[128];
			char crt_file[128];
			char key_file[128];

			if( sscanf( val, "%127[^,],%127[^,],%127[^,]", name, crt_file, key_file ) == 3 ) {
				tls_add_sni_entry( name, crt_file, key_file );
			} else {
				log_err( "CFG: Invalid option format: %s", val );
				exit(1);
			}
			break;
		}
#endif
#ifdef WEB
		case oWebPort:
			conf_str( opt, &gconf->web_port, val );
			break;
#endif
		case oConfig:
			if( val == NULL ) {
				conf_arg_expected( opt );
				break;
			}
			conf_load_file( val );
			conf_str( opt, &gconf->configfile, val );
			break;
		case oMode:
			if( val == NULL ) {
				conf_arg_expected( opt );
				break;
			} else if( gconf->af != 0 ) {
				conf_duplicate_option( opt );
			} else if( match( val, "ipv4" ) ) {
				gconf->af = AF_INET;
			} else if( match( val, "ipv6" ) ) {
				gconf->af = AF_INET6;
			} else {
				log_err("CFG: Invalid argument for %s. Use 'ipv4' or 'ipv6'.", opt );
				exit( 1 );
			}
			break;
		case oPort:
			conf_str( opt, &gconf->dht_port, val );
			break;
		case oAddr:
			conf_str( opt, &gconf->dht_addr, val );
			break;
#ifdef LPD
		case oLpdAddr:
			conf_str( opt, &gconf->lpd_addr, val );
			break;
		case oLpdDisable:
			if( val != NULL ) {
				conf_no_arg_expected( opt );
			} else {
				gconf->lpd_disable = 1;
			}
			break;
#endif
#ifdef FWD
		case oFwdDisable:
			if( val != NULL ) {
				conf_no_arg_expected( opt );
			} else {
				gconf->fwd_disable = 1;
			}
			break;
#endif
#ifdef __CYGWIN__
		case oServiceInstall:
			if( val != NULL ) {
				conf_no_arg_expected( opt );
			} else {
				windows_service_install();
				exit(0);
			}
			break;
		case oServiceRemove:
			if( val != NULL ) {
				conf_no_arg_expected( opt );
			} else {
				windows_service_remove();
				exit(0);
			}
			break;
		case oServiceStart:
			if( val != NULL ) {
				conf_no_arg_expected( opt );
			} else {
				gconf->service_start = 1;
			}
			break;
#endif
		case oIfname:
			conf_str( opt, &gconf->dht_ifname, val );
			break;
		case oUser:
			conf_str( opt, &gconf->user, val );
			break;
		case oDaemon:
			if( val != NULL ) {
				conf_no_arg_expected( opt );
			} else {
				gconf->is_daemon = 1;
			}
			break;
		case oHelp:
			printf( "%s\n", kadnode_usage_str );
			exit( 0 );
			break;
		case oVersion:
			printf( "%s\n", kadnode_version_str );
			exit( 0 );
			break;
#ifdef BOB
		case oBobGenKeys:
			exit( bob_generate_key_pair() );
			break;
		case oBobAddSkey:
			if( val == NULL ) {
				conf_arg_expected( opt );
				return;
			}
			if( bob_add_skey( val ) < 0 ) {
				printf( "Invalid secret key: %s\n", val );
				exit( 1 );
			}
			break;
#endif
// Dependend on other settings? 
		case oValueId:
			if( val == NULL ) {
				conf_arg_expected( opt );
				return;
			}
			conf_apply_value( val );
			break;
		default:
			log_err( "CFG: Unkown option: %s", opt );
			exit(1);
	}
}

void conf_append(const char opt[], const char val[]) {
	// Account for both new entries and NULL delimiter
	g_argv = (char**) realloc(g_argv, (g_argc + 3) * sizeof(char*));
	g_argv[g_argc] = opt ? strdup(opt) : NULL;
	g_argv[g_argc + 1] = val ? strdup(val) : NULL;
	g_argv[g_argc + 2] = NULL;
	g_argc += 2;
}

void conf_load_file( const char filename[] ) {
	char line[512];
	size_t n;
	struct stat s;
	FILE *file;
	char *option;
	char *value;
	char *p;

	if( stat( filename, &s ) == 0 && !(s.st_mode & S_IFREG) ) {
		log_err( "CFG: File expected: %s\n", filename );
		exit( 1 );
	}

	n = 0;
	file = fopen( filename, "r" );
	if( file == NULL ) {
		log_err( "CFG: Cannot open file '%s': %s\n", filename, strerror( errno ) );
		exit( 1 );
	}

	while( fgets( line, sizeof(line), file ) != NULL ) {
		n++;
		option = NULL;
		value = NULL;

		// End line early at '#'
		if( (p = strchr( line, '#' )) != NULL ) {
			*p =  '\0';
		}

		// Replace quotation marks with spaces
		p = line;
		while( *p ) {
			if( *p == '\'' || *p == '\"' ) {
				*p = ' ';
			}
			p++;
		}

		// Parse "--option [<value>]"
		char *pch = strtok( line," \t\n\r" );
		while( pch != NULL ) {
			if( option == NULL ) {
				option = pch;
			} else if( value == NULL ) {
				value = pch;
			} else {
				fclose( file );
				log_err( "CFG: Too many arguments in line %ld.", n );
				exit( 1 );
			}
			pch = strtok( NULL, " \t\n\r" );
		}

		if( option == NULL ) {
			continue;
		}

		if( strcmp( option, "--config" ) == 0 ) {
			fclose( file );
			log_err( "CFG: Option '--config' not allowed inside a configuration file, line %ld.", n );
			exit( 1 );
		}

		conf_append( option, value );
	}

	fclose( file );
}


void conf_load_args( int argc, char **argv ) {
	int i;

	g_argv = (char**) memdup(argv, argc * sizeof(char*));
	g_argc = argc;

	for( i = 1; i < g_argc; i++ ) {
		const char *opt = g_argv[i];
		const char *val = g_argv[i+1];
		if( opt ) {
			if( val && val[0] != '-') {
				// -x abc
				conf_handle_option( opt, val );
				i++;
			} else {
				// -x
				conf_handle_option( opt, NULL );
			}
		}
	}

	conf_check();
}
