/*
 * "$Id$"
 *
 *   Sample IPP server for CUPS.
 *
 *   Copyright 2010 by Apple Inc.
 *
 *   These coded instructions, statements, and computer programs are the
 *   property of Apple Inc. and are protected by Federal copyright
 *   law.  Distribution and use rights are outlined in the file "LICENSE.txt"
 *   which should have been included with this file.  If this file is
 *   file is missing or damaged, see the license at "http://www.cups.org/".
 *
 * Contents:
 *
 */

/*
 * Include necessary headers...
 */

#include <cups/cups-private.h>
#include <dns_sd.h>
#include <sys/stat.h>
#ifdef HAVE_SYS_MOUNT_H
#  include <sys/mount.h>
#endif /* HAVE_SYS_MOUNT_H */
#ifdef HAVE_SYS_STATFS_H
#  include <sys/statfs.h>
#endif /* HAVE_SYS_STATFS_H */
#ifdef HAVE_SYS_STATVFS_H
#  include <sys/statvfs.h>
#endif /* HAVE_SYS_STATVFS_H */
#ifdef HAVE_SYS_VFS_H
#  include <sys/vfs.h>
#endif /* HAVE_SYS_VFS_H */


/*
 * Constants...
 */

enum _ipp_preasons_e			/* printer-state-reasons bit values */
{
  _IPP_PRINTER_NONE = 0x0000,		/* none */
  _IPP_PRINTER_OTHER = 0x0001,		/* other */
  _IPP_PRINTER_COVER_OPEN = 0x0002,	/* cover-open */
  _IPP_PRINTER_INPUT_TRAY_MISSING = 0x0004,
					/* input-tray-missing */
  _IPP_PRINTER_MARKER_SUPPLY_EMPTY = 0x0008,
					/* marker-supply-empty */
  _IPP_PRINTER_MARKER_SUPPLY_LOW = 0x0010,
					/* marker-suply-low */
  _IPP_PRINTER_MARKER_WASTE_ALMOST_FULL = 0x0020,
					/* marker-waste-almost-full */
  _IPP_PRINTER_MARKER_WASTE_FULL = 0x0040,
					/* marker-waste-full */
  _IPP_PRINTER_MEDIA_EMPTY = 0x0080,	/* media-empty */
  _IPP_PRINTER_MEDIA_JAM = 0x0100,	/* media-jam */
  _IPP_PRINTER_MEDIA_LOW = 0x0200,	/* media-low */
  _IPP_PRINTER_MEDIA_NEEDED = 0x0400,	/* media-needed */
  _IPP_PRINTER_MOVING_TO_PAUSED = 0x0800,
					/* moving-to-paused */
  _IPP_PRINTER_PAUSED = 0x1000,		/* paused */
  _IPP_PRINTER_SPOOL_AREA_FULL = 0x2000,/* spool-area-full */
  _IPP_PRINTER_TONER_EMPTY = 0x4000,	/* toner-empty */
  _IPP_PRINTER_TONER_LOW = 0x8000	/* toner-low */
};
typedef unsigned int _ipp_preasons_t;	/* Bitfield for printer-state-reasons */


/*
 * Structures...
 */

typedef struct _ipp_printer_s		/**** Printer data ****/
{
  int			ipv4,		/* IPv4 listener */
			ipv6;		/* IPv6 listener */
  DNSServiceRef		common_ref,	/* Shared service connection */
			ipp_ref,	/* Bonjour IPP service */
			printer_ref;	/* Bonjour LPD service */
  TXTRecordRef		ipp_txt;	/* Bonjour IPP TXT record */
  char			*name,		/* printer-name */
			*dnssd_name,	/* printer-dnssd-name */
			*icon,		/* Icon filename */
			*directory,	/* Spool directory */
			*hostname;	/* Hostname */
  int			port;		/* Port */
  ipp_t			*attrs;		/* Static attributes */
  ipp_pstate_t		state;		/* printer-state value */
  _ipp_preasons_t	state_reasons;	/* printer-state-reasons values */  
  cups_array_t		*jobs;		/* Jobs */
  int			next_job_id;	/* Next job-id value */
  _cups_rwlock_t	rwlock;		/* Printer lock */
} _ipp_printer_t;

typedef struct _ipp_job_s		/**** Job data ****/
{
  int			id,		/* Job ID */
			use;		/* Use count */
  char			*name;		/* job-name */
  ipp_jstate_t		state;		/* job-state value */
  time_t		completed;	/* time-at-completed value */
  ipp_t			*attrs;		/* Static attributes */
  int			canceled;	/* Non-zero when job canceled */
  char			*filename;	/* Print file name */
  int			fd;		/* Print file descriptor */
  _ipp_printer_t	*printer;	/* Printer */
} _ipp_job_t;

typedef struct _ipp_client_s		/**** Client data ****/
{
  http_t		http;		/* HTTP connection */
  ipp_t			*request,	/* IPP request */
			*response;	/* IPP response */
  time_t		start;		/* Request start time */
  http_state_t		operation;	/* Request operation */
  char			uri[1024];	/* Request URI */
  _ipp_printer_t	*printer;	/* Printer */
  _ipp_job_t		*job;		/* Current job, if any */
} _ipp_client_t;


/*
 * Local functions...
 */

static void		clean_jobs(_ipp_printer_t *printer);
static int		compare_jobs(_ipp_job_t *a, _ipp_job_t *b);
static ipp_attribute_t	*copy_attr(ipp_t *to, ipp_attribute_t *attr,
		                   ipp_tag_t group_tag, int quickcopy);
static void		copy_attrs(ipp_t *to, ipp_t *from, cups_array_t *ra,
			           ipp_tag_t group_tag, int quickcopy);
static void		copy_job_attrs(_ipp_client_t *client, _ipp_job_t *job,
			               cups_array_t *ra);
static _ipp_client_t	*create_client(_ipp_printer_t *printer, int fd);
static _ipp_job_t	*create_job(_ipp_client_t *client);
static int		create_listener(int family, int *port);
static ipp_t		*create_media_col(const char *media, const char *type,
					  int width, int length, int margins);
static _ipp_printer_t	*create_printer(const char *name, const char *location,
			                const char *make, const char *model,
					const char *icon,
					const char *docformats, int ppm,
					int ppm_color, int duplex, int port,
					const char *regtype,
					const char *directory);
static cups_array_t	*create_requested_array(_ipp_client_t *client);
static void		delete_client(_ipp_client_t *client);
static void		delete_job(_ipp_job_t *job);
static void		delete_printer(_ipp_printer_t *printer);
static void		dnssd_callback(DNSServiceRef sdRef,
				       DNSServiceFlags flags,
				       DNSServiceErrorType errorCode,
				       const char *name,
				       const char *regtype,
				       const char *domain,
				       _ipp_printer_t *printer);
static void		ipp_cancel_job(_ipp_client_t *client);
static void		ipp_create_job(_ipp_client_t *client);
static void		ipp_get_job_attributes(_ipp_client_t *client);
static void		ipp_get_jobs(_ipp_client_t *client);
static void		ipp_get_printer_attributes(_ipp_client_t *client);
static void		ipp_print_job(_ipp_client_t *client);
static void		ipp_send_document(_ipp_client_t *client);
static void		ipp_validate_job(_ipp_client_t *client);
static void		*process_client(_ipp_client_t *client);
static int		process_http(_ipp_client_t *client);
static int		process_ipp(_ipp_client_t *client);
static int		register_printer(_ipp_printer_t *printer,
			                 const char *location, const char *make,
					 const char *model, const char *formats,
					 const char *adminurl, int color,
					 int duplex, const char *regtype);
static int		respond_http(_ipp_client_t *client, http_status_t code,
				     const char *type, size_t length);
static void		respond_ipp(_ipp_client_t *client, ipp_status_t status,
			            const char *message, ...)
#ifdef __GNUC__
__attribute__ ((__format__ (__printf__, 3, 4)))
#endif /* __GNUC__ */
;
static void		run_printer(_ipp_printer_t *printer);
static void		usage(int status);


/*
 * 'main()' - Main entry to the sample server.
 */

int					/* O - Exit status */
main(int  argc,				/* I - Number of command-line args */
     char *argv[])			/* I - Command-line arguments */
{
  int		i;			/* Looping var */
  const char	*opt,			/* Current option character */
		*name = NULL,		/* Printer name */
		*location = "",		/* Location of printer */
		*make = "Test",		/* Manufacturer */
		*model = "Printer",	/* Model */
		*icon = "printer.png",	/* Icon file */
		*formats = "application/pdf,image/jpeg",
	      				/* Supported formats */
		*regtype = "_ipp._tcp";	/* Bonjour service type */
  int		port = 0,		/* Port number (0 = auto) */
		duplex = 0,		/* Duplex mode */
		ppm = 10,		/* Pages per minute for mono */
		ppm_color = 0;		/* Pages per minute for color */
  char		directory[1024] = "";	/* Spool directory */
  _ipp_printer_t *printer;		/* Printer object */


 /*
  * Parse command-line arguments...
  */

  for (i = 1; i < argc; i ++)
    if (argv[i][0] == '-')
    {
      for (opt = argv[i] + 1; *opt; opt ++)
        switch (*opt)
	{
	  case '2' : /* -2 (enable 2-sided printing) */
	      duplex = 1;
	      break;

	  case 'M' : /* -M manufacturer */
	      i ++;
	      if (i >= argc)
	        usage(1);
	      make = argv[i];
	      break;

	  case 'd' : /* -d spool-directory */
	      i ++;
	      if (i >= argc)
	        usage(1);
	      strlcpy(directory, argv[i], sizeof(directory));
	      break;

	  case 'f' : /* -f type/subtype[,...] */
	      i ++;
	      if (i >= argc)
	        usage(1);
	      formats = argv[i];
	      break;

          case 'h' : /* -h (show help) */
	      usage(0);
	      break;

	  case 'i' : /* -i icon.png */
	      i ++;
	      if (i >= argc)
	        usage(1);
	      icon = argv[i];
	      break;

	  case 'l' : /* -l location */
	      i ++;
	      if (i >= argc)
	        usage(1);
	      location = argv[i];
	      break;

	  case 'm' : /* -m model */
	      i ++;
	      if (i >= argc)
	        usage(1);
	      model = argv[i];
	      break;

	  case 'p' : /* -p port */
	      i ++;
	      if (i >= argc || !isdigit(argv[i][0] & 255))
	        usage(1);
	      port = atoi(argv[i]);
	      break;

	  case 'r' : /* -r regtype */
	      i ++;
	      if (i >= argc)
	        usage(1);
	      regtype = argv[i];
	      break;

	  case 's' : /* -s speed[,color-speed] */
	      i ++;
	      if (i >= argc)
	        usage(1);
	      if (sscanf(argv[i], "%d,%d", &ppm, &ppm_color) < 1)
	        usage(1);
	      break;

          default : /* Unknown */
	      fprintf(stderr, "Unknown option \"-%c\".\n", *opt);
	      usage(1);
	      break;
	}
    }
    else if (!name)
    {
      name = argv[i];
    }
    else
    {
      fprintf(stderr, "Unexpected command-line argument \"%s\"\n", argv[i]);
      usage(1);
    }

  if (!name)
    usage(1);

 /*
  * Apply defaults as needed...
  */

  if (!directory[0])
  {
    snprintf(directory, sizeof(directory), "/tmp/ippserver.%d", (int)getpid());

    if (mkdir(directory, 0777) && errno != EEXIST)
    {
      fprintf(stderr, "Unable to create spool directory \"%s\": %s\n",
	      directory, strerror(errno));
      usage(1);
    }

    printf("Using spool directory \"%s\".\n", directory);
  }

 /*
  * Create the printer...
  */

  if ((printer = create_printer(name, location, make, model, icon, formats, ppm,
                                ppm_color, duplex, port, regtype,
				directory)) == NULL)
    return (1);

 /*
  * Run the print service...
  */

  run_printer(printer);

 /*
  * Destroy the printer and exit...
  */

  delete_printer(printer);

  return (0);
}


/*
 * 'clean_jobs()' - Clean out old (completed) jobs.
 */

static void
clean_jobs(_ipp_printer_t *printer)	/* I - Printer */
{
}


/*
 * 'compare_jobs()' - Compare two jobs.
 */

static int				/* O - Result of comparison */
compare_jobs(_ipp_job_t *a,		/* I - First job */
             _ipp_job_t *b)		/* I - Second job */
{
  return (b->id - a->id);
}


/*
 * 'copy_attr()' - Copy a single attribute.
 */

static ipp_attribute_t *		/* O - New attribute */
copy_attr(ipp_t           *to,		/* O - Destination request/response */
          ipp_attribute_t *attr,	/* I - Attribute to copy */
          ipp_tag_t       group_tag,	/* I - Group to put the copy in */
          int             quickcopy)	/* I - Do a quick copy? */
{
  int			i;		/* Looping var */
  ipp_attribute_t	*toattr;	/* Destination attribute */


  switch (attr->value_tag & ~IPP_TAG_COPY)
  {
    case IPP_TAG_ZERO :
        toattr = ippAddSeparator(to);
	break;

    case IPP_TAG_INTEGER :
    case IPP_TAG_ENUM :
        toattr = ippAddIntegers(to, group_tag, attr->value_tag,
	                        attr->name, attr->num_values, NULL);

        for (i = 0; i < attr->num_values; i ++)
	  toattr->values[i].integer = attr->values[i].integer;
        break;

    case IPP_TAG_BOOLEAN :
        toattr = ippAddBooleans(to, group_tag, attr->name,
	                        attr->num_values, NULL);

        for (i = 0; i < attr->num_values; i ++)
	  toattr->values[i].boolean = attr->values[i].boolean;
        break;

    case IPP_TAG_TEXT :
    case IPP_TAG_NAME :
    case IPP_TAG_KEYWORD :
    case IPP_TAG_URI :
    case IPP_TAG_URISCHEME :
    case IPP_TAG_CHARSET :
    case IPP_TAG_LANGUAGE :
    case IPP_TAG_MIMETYPE :
        toattr = ippAddStrings(to, group_tag,
	                       (ipp_tag_t)(attr->value_tag | quickcopy),
	                       attr->name, attr->num_values, NULL, NULL);

        if (quickcopy)
	{
          for (i = 0; i < attr->num_values; i ++)
	    toattr->values[i].string.text = attr->values[i].string.text;
        }
	else
	{
          for (i = 0; i < attr->num_values; i ++)
	    toattr->values[i].string.text =
	        _cupsStrAlloc(attr->values[i].string.text);
	}
        break;

    case IPP_TAG_DATE :
        toattr = ippAddDate(to, group_tag, attr->name,
	                    attr->values[0].date);
        break;

    case IPP_TAG_RESOLUTION :
        toattr = ippAddResolutions(to, group_tag, attr->name,
	                           attr->num_values, IPP_RES_PER_INCH,
				   NULL, NULL);

        for (i = 0; i < attr->num_values; i ++)
	{
	  toattr->values[i].resolution.xres  = attr->values[i].resolution.xres;
	  toattr->values[i].resolution.yres  = attr->values[i].resolution.yres;
	  toattr->values[i].resolution.units = attr->values[i].resolution.units;
	}
        break;

    case IPP_TAG_RANGE :
        toattr = ippAddRanges(to, group_tag, attr->name,
	                      attr->num_values, NULL, NULL);

        for (i = 0; i < attr->num_values; i ++)
	{
	  toattr->values[i].range.lower = attr->values[i].range.lower;
	  toattr->values[i].range.upper = attr->values[i].range.upper;
	}
        break;

    case IPP_TAG_TEXTLANG :
    case IPP_TAG_NAMELANG :
        toattr = ippAddStrings(to, group_tag,
	                       (ipp_tag_t)(attr->value_tag | quickcopy),
	                       attr->name, attr->num_values, NULL, NULL);

        if (quickcopy)
	{
          for (i = 0; i < attr->num_values; i ++)
	  {
            toattr->values[i].string.charset = attr->values[i].string.charset;
	    toattr->values[i].string.text    = attr->values[i].string.text;
          }
        }
	else
	{
          for (i = 0; i < attr->num_values; i ++)
	  {
	    if (!i)
              toattr->values[i].string.charset =
	          _cupsStrAlloc(attr->values[i].string.charset);
	    else
              toattr->values[i].string.charset =
	          toattr->values[0].string.charset;

	    toattr->values[i].string.text =
	        _cupsStrAlloc(attr->values[i].string.text);
          }
        }
        break;

    case IPP_TAG_BEGIN_COLLECTION :
        toattr = ippAddCollections(to, group_tag, attr->name,
	                           attr->num_values, NULL);

        for (i = 0; i < attr->num_values; i ++)
	{
	  toattr->values[i].collection = attr->values[i].collection;
	  attr->values[i].collection->use ++;
	}
        break;

    case IPP_TAG_STRING :
        if (quickcopy)
	{
	  toattr = ippAddOctetString(to, group_tag, attr->name, NULL, 0);
	  toattr->value_tag |= quickcopy;
	  toattr->values[0].unknown.data   = attr->values[0].unknown.data;
	  toattr->values[0].unknown.length = attr->values[0].unknown.length;
	}
	else
	  toattr = ippAddOctetString(to, attr->group_tag, attr->name,
				     attr->values[0].unknown.data,
				     attr->values[0].unknown.length);
        break;

    default :
        toattr = ippAddIntegers(to, group_tag, attr->value_tag,
	                        attr->name, attr->num_values, NULL);

        for (i = 0; i < attr->num_values; i ++)
	{
	  toattr->values[i].unknown.length = attr->values[i].unknown.length;

	  if (toattr->values[i].unknown.length > 0)
	  {
	    if ((toattr->values[i].unknown.data =
	             malloc(toattr->values[i].unknown.length)) == NULL)
	      toattr->values[i].unknown.length = 0;
	    else
	      memcpy(toattr->values[i].unknown.data,
		     attr->values[i].unknown.data,
		     toattr->values[i].unknown.length);
	  }
	}
        break; /* anti-compiler-warning-code */
  }

  return (toattr);
}


/*
 * 'copy_attrs()' - Copy attributes from one request to another.
 */

static void
copy_attrs(ipp_t        *to,		/* I - Destination request */
	   ipp_t        *from,		/* I - Source request */
	   cups_array_t *ra,		/* I - Requested attributes */
	   ipp_tag_t    group_tag,	/* I - Group to copy */
	   int          quickcopy)	/* I - Do a quick copy? */
{
  ipp_attribute_t	*fromattr;	/* Source attribute */


  if (!to || !from)
    return;

  for (fromattr = from->attrs; fromattr; fromattr = fromattr->next)
  {
   /*
    * Filter attributes as needed...
    */

    if ((group_tag != IPP_TAG_ZERO && fromattr->group_tag != group_tag &&
         fromattr->group_tag != IPP_TAG_ZERO) || !fromattr->name)
      continue;

    if (!ra || cupsArrayFind(ra, fromattr->name))
      copy_attr(to, fromattr, quickcopy, fromattr->group_tag);
  }
}


/*
 * 'copy_job_attrs()' - Copy job attributes to the response.
 */

static void
copy_job_attrs(_ipp_client_t *client,	/* I - Client */
               _ipp_job_t    *job,	/* I - Job */
	       cups_array_t  *ra)	/* I - requested-attributes */
{
  if (!ra || cupsArrayFind(ra, "job-printer-up-time"))
    ippAddInteger(client->response, IPP_TAG_JOB, IPP_TAG_INTEGER,
                  "job-printer-up-time", (int)time(NULL));

  if (!ra || cupsArrayFind(ra, "job-state"))
    ippAddInteger(client->response, IPP_TAG_JOB, IPP_TAG_ENUM,
		  "job-state", job->state);

  if (!ra || cupsArrayFind(ra, "job-state-reasons"))
  {
    switch (job->state)
    {
      case IPP_JOB_PENDING :
	  ippAddString(client->response, IPP_TAG_JOB, IPP_TAG_KEYWORD,
		       "job-state-reasons", NULL, "none");
	  break;

      case IPP_JOB_HELD :
	  if (ippFindAttribute(job->attrs, "job-hold-until", IPP_TAG_ZERO))
	    ippAddString(client->response, IPP_TAG_JOB, IPP_TAG_KEYWORD,
			 "job-state-reasons", NULL, "job-hold-until-specified");
	  else
	    ippAddString(client->response, IPP_TAG_JOB, IPP_TAG_KEYWORD,
			 "job-state-reasons", NULL, "job-incoming");
	  break;

      case IPP_JOB_PROCESSING :
	  if (job->canceled)
	    ippAddString(client->response, IPP_TAG_JOB, IPP_TAG_KEYWORD,
			 "job-state-reasons", NULL, "processing-to-stop-point");
	  else
	    ippAddString(client->response, IPP_TAG_JOB, IPP_TAG_KEYWORD,
			 "job-state-reasons", NULL, "job-printing");
	  break;

      case IPP_JOB_STOPPED :
	  ippAddString(client->response, IPP_TAG_JOB, IPP_TAG_KEYWORD,
		       "job-state-reasons", NULL, "job-stopped");
	  break;

      case IPP_JOB_CANCELED :
	  ippAddString(client->response, IPP_TAG_JOB, IPP_TAG_KEYWORD,
		       "job-state-reasons", NULL, "job-canceled-by-user");
	  break;

      case IPP_JOB_ABORTED :
	  ippAddString(client->response, IPP_TAG_JOB, IPP_TAG_KEYWORD,
		       "job-state-reasons", NULL, "aborted-by-system");
	  break;

      case IPP_JOB_COMPLETED :
	  ippAddString(client->response, IPP_TAG_JOB, IPP_TAG_KEYWORD,
		       "job-state-reasons", NULL, "job-completed-successfully");
	  break;
    }
  }

  copy_attrs(client->response, job->attrs, ra, 0, IPP_TAG_ZERO);
}


/*
 * 'create_client()' - Accept a new network connection and create a client
 *                     object.
 */

static _ipp_client_t *			/* O - Client */
create_client(_ipp_printer_t *printer,	/* I - Printer */
              int            fd)	/* I - Listen socket */
{
  _ipp_client_t	*client;		/* Client */
  int		val;			/* Parameter value */
  socklen_t	addrlen;		/* Length of address */


  if ((client = calloc(1, sizeof(_ipp_client_t))) == NULL)
  {
    perror("Unable to allocate memory for client");
    return (NULL);
  }

  client->http.activity = time(NULL);
  client->http.hostaddr = &(client->clientaddr);
  client->http.blocking = 1;

 /*
  * Accept the client and get the remote address...
  */

  addrlen = sizeof(http_addr_t);

  if ((client->http.fd = accept(sock, (struct sockaddr *)client->http.hostaddr,
                                &addrlen)) < 0)
  {
    perror("Unable to accept client connection");

    free(client);

    return (NULL);
  }

  httpAddrString(client->http.hostaddr, client->http.hostname,
		 sizeof(client->http.hostname));

  fprintf(stderr, "Accepted connection from %s:%d (%s)",
	  client->http.hostname, _httpAddrPort(client->http.hostaddr),
	  client->http.hostaddr->addr.sa_family == AF_INET ? "IPv4" : "IPv6");

 /*
  * Using TCP_NODELAY improves responsiveness, especially on systems
  * with a slow loopback interface.  Since we write large buffers
  * when sending print files and requests, there shouldn't be any
  * performance penalty for this...
  */

  val = 1;
  setsockopt(client->http.fd, IPPROTO_TCP, TCP_NODELAY, (char *)&val,
             sizeof(val));

  return (client);
}


/*
 * 'create_job()' - Create a new job object from a Print-Job or Create-Job
 *                  request.
 */

static _ipp_job_t *			/* O - Job */
create_job(_ipp_client_t *client)	/* I - Client */
{
}


/*
 * 'create_listener()' - Create a listener socket.
 */

static int				/* O  - Listener socket or -1 on error */
create_listener(int family,		/* I  - Address family */
                int *port)		/* IO - Port number */
{
  int		sock,			/* Listener socket */
		val;			/* Socket value */
  http_addr_t	address;		/* Listen address */
  socklen_t	addrlen;		/* Length of listen address */


  if ((sock = socket(family, SOCK_STREAM, 0)) < 0)
    return (-1);

  val = 1;
  setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));

#ifdef IPV6_V6ONLY
  if (family == AF_INET6)
    setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, &val, sizeof(val));
#endif /* IPV6_V6ONLY */

  if (!port)
  {
   /*
    * Get the auto-assigned port number for the IPv4 socket...
    */

    addrlen = sizeof(address);
    if (getsockname(sock, (struct sockaddr *)&address, &addrlen))
      *port = 8631;
    else if (family == AF_INET)
      *port = ntohl(address.ipv4.sin_port);
    else
      *port = ntohl(address.ipv6.sin6_port);
  }

  memset(&address, 0, sizeof(address));
  if (family == AF_INET)
  {
    address.ipv4.sin_family = family;
    address.ipv4.sin_port   = htonl(*port);
  }
  else
  {
    address.ipv6.sin6_family = family;
    address.ipv6.sin6_port   = htonl(*port);
  }

  if (bind(sock, (struct sockaddr *)&address, httpAddrLength(&address)))
  {
    close(sock);
    return (-1);
  }

  if (listen(sock, 5))
  {
    close(sock);
    return (-1);
  }

  return (sock);
}


/*
 * 'create_media_col()' - Create a media-col value.
 */

static ipp_t *				/* O - media-col collection */
create_media_col(const char *media,	/* I - Media name */
		 const char *type,	/* I - Nedua type */
		 int        width,	/* I - x-dimension in 2540ths */
		 int        length,	/* I - y-dimension in 2540ths */
		 int        margins)	/* I - Value for margins */
{
  ipp_t	*media_col = ippNew(),		/* media-col value */
	*media_size = ippNew();		/* media-size value */
  char	media_key[256];			/* media-key value */


  ippAddInteger(media_size, IPP_TAG_PRINTER, IPP_TAG_INTEGER, "x-dimension",
                width);
  ippAddInteger(media_size, IPP_TAG_PRINTER, IPP_TAG_INTEGER, "y-dimension",
                length);

  snprintf(media_key, sizeof(media_key), "%s_%s", media, type);

  ippAddString(media_col, IPP_TAG_PRINTER, IPP_TAG_KEYWORD, "media-key", NULL,
               media_key);
  ippAddCollection(media_col, IPP_TAG_PRINTER, "media-size", media_size);
  ippAddInteger(media_col, IPP_TAG_PRINTER, IPP_TAG_INTEGER,
                "media-bottom-margin", margins);
  ippAddInteger(media_col, IPP_TAG_PRINTER, IPP_TAG_INTEGER,
                "media-left-margin", margins);
  ippAddInteger(media_col, IPP_TAG_PRINTER, IPP_TAG_INTEGER,
                "media-right-margin", margins);
  ippAddInteger(media_col, IPP_TAG_PRINTER, IPP_TAG_INTEGER,
                "media-top-margin", margins);
  ippAddString(media_col, IPP_TAG_PRINTER, IPP_TAG_KEYWORD, "media-type",
               NULL, type);

  ippDelete(media_size);

  return (media_col);
}


/*
 * 'create_printer()' - Create, register, and listen for connections to a
 *                      printer object.
 */

static _ipp_printer_t *			/* O - Printer */
create_printer(const char *name,	/* I - printer-name */
	       const char *location,	/* I - printer-location */
	       const char *make,	/* I - printer-make-and-model */
	       const char *model,	/* I - printer-make-and-model */
	       const char *icon,	/* I - printer-icons */
	       const char *docformats,	/* I - document-format-supported */
	       int        ppm,		/* I - Pages per minute in grayscale */
	       int        ppm_color,	/* I - Pages per minute in color (0 for gray) */
	       int        duplex,	/* I - 1 = duplex, 0 = simplex */
	       int        port,		/* I - Port for listeners or 0 for auto */
	       const char *regtype,	/* I - Bonjour service type */
	       const char *directory)	/* I - Spool directory */
{
  int			i, j, k;	/* Looping vars */
  _ipp_printer_t	*printer;	/* Printer */
  char			hostname[256],	/* Hostname */
			uri[1024],	/* Printer URI */
			icons[1024],	/* printer-icons URI */
			adminurl[1024],	/* printer-more-info URI */
			device_id[1024],/* printer-device-id */
			make_model[128];/* printer-make-and-model */
  int			num_formats;	/* Number of document-format-supported values */
  char			*defformat,	/* document-format-default value */
			*formats[100],	/* document-format-supported values */
			*ptr;		/* Pointer into string */
  const char		*prefix;	/* Prefix string */
  int			num_database;	/* Number of database values */
  ipp_attribute_t	*media_col_database;
					/* media-col-database value */
  ipp_t			*media_col_default;
					/* media-col-default value */
  ipp_value_t		*media_col_value;
					/* Current media-col-database value */
  int			k_supported;	/* Maximum file size supported */
#ifdef HAVE_STATFS
  struct statfs		spoolinfo;	/* FS info for spool directory */
  double		spoolsize;	/* FS size */
#elif defined(HAVE_STATVFS)
  struct statvfs	spoolinfo;	/* FS info for spool directory */
  double		spoolsize;	/* FS size */
#endif /* HAVE_STATFS */
  static const int	orients[4] =	/* orientation-requested-supported values */
  {
    IPP_PORTRAIT,
    IPP_LANDSCAPE,
    IPP_REVERSE_LANDSCAPE,
    IPP_REVERSE_PORTRAIT
  };
  static const char * const versions[] =/* ipp-versions-supported values */
  {
    "1.0",
    "1.1",
    "2.0"
  };
  static const int	ops[] =		/* operations-supported values */
  {
    IPP_PRINT_JOB,
    IPP_VALIDATE_JOB,
    IPP_CREATE_JOB,
    IPP_SEND_DOCUMENT,
    IPP_CANCEL_JOB,
    IPP_GET_JOB_ATTRIBUTES,
    IPP_GET_JOBS,
    IPP_GET_PRINTER_ATTRIBUTES
  };
  static const char * const charsets[] =/* charset-supported values */
  {
    "us-ascii",
    "utf-8"
  };
  static const char * const job_creation[] =
  {					/* job-creation-attributes-supported values */
    "copies",
    "ipp-attribute-fidelity",
    "job-name",
    "media",
    "media-col",
    "multiple-document-handling",
    "output-bin",
    "orientation-requested",
    "print-quality",
    "printer-resolution",
    "sides"
  };
  static const char * const media_supported[] =
  {					/* media-supported values */
    "iso_a4_210x297mm",			/* A4 */
    "iso_a5_148x210mm",			/* A5 */
    "iso_a6_105x148mm",			/* A6 */
    "iso_dl_110x220mm",			/* DL */
    "na_legal_8.5x14in",		/* Legal */
    "na_letter_8.5x11in",		/* Letter */
    "na_number-10_4.125x9.5in",		/* #10 */
    "na_index-3x5_3x5in",		/* 3x5 */
    "oe_photo-l_3.5x5in",		/* L */
    "na_index-4x6_4x6in",		/* 4x6 */
    "na_5x7_5x7in"			/* 5x7 */
  };
#define _IPP_GENERAL	0		/* General-purpose size */
#define _IPP_PHOTO_ONLY	1		/* Photo-only size */
#define _IPP_ENV_ONLY	2		/* Envelope-only size */
  static const int media_col_sizes[][3] =
  {					/* media-col-database sizes */
    { 21000, 29700, _IPP_GENERAL },	/* A4 */
    { 14800, 21000, _IPP_PHOTO_ONLY },	/* A5 */
    { 10500, 14800, _IPP_PHOTO_ONLY },	/* A6 */
    { 11000, 22000, _IPP_ENV_ONLY },	/* DL */
    { 21590, 35560, _IPP_GENERAL },	/* Legal */
    { 21590, 27940, _IPP_GENERAL },	/* Letter */
    { 10477, 24130, _IPP_ENV_ONLY },	/* #10 */
    {  7630, 12700, _IPP_PHOTO_ONLY },	/* 3x5 */
    {  8890, 12700, _IPP_PHOTO_ONLY },	/* L */
    { 10160, 15240, _IPP_PHOTO_ONLY },	/* 4x6 */
    { 12700, 17780, _IPP_PHOTO_ONLY }	/* 5x7 */
  };
  static const char * const media_col_supported[] =
  {					/* media-col-supported values */
    "media-bottom-margin",
    "media-left-margin",
    "media-right-margin",
    "media-size",
    "media-top-margin",
    "media-type"
  };
  static const char * const media_type_supported[] =
					/* media-type-supported values */
  {
    "auto",
    "cardstock",
    "envelope",
    "labels",
    "other",
    "photographic-glossy",
    "photographic-high-gloss",
    "photographic-matte",
    "photographic-satin",
    "photographic-semi-gloss",
    "stationery",
    "stationery-letterhead",
    "transparency"
  };
  static const int	media_xxx_margin_supported[] =
  {					/* media-xxx-margin-supported values */
    0,
    635
  };
  static const char * const multiple_document_handling[] =
  {					/* multiple-document-handling-supported values */
    "separate-documents-uncollated-copies",
    "separate-documents-collated-copies"
  };
  static const int	print_quality_supported[] =
  {					/* print-quality-supported values */
    IPP_QUALITY_DRAFT,
    IPP_QUALITY_NORMAL,
    IPP_QUALITY_HIGH
  };
  static const char * const sides_supported[] =
  {					/* sides-supported values */
    "one-sided",
    "two-sided-long-edge",
    "two-sided-short-edge"
  };
  static const char * const which_jobs[] =
  {					/* which-jobs-supported values */
    "completed",
    "not-completed",
    "aborted",
    "all",
    "canceled",
    "pending",
    "pending-held",
    "processing",
    "processing-stopped"
  };


 /*
  * Allocate memory for the printer...
  */

  if ((printer = calloc(1, sizeof(_ipp_printer_t))) == NULL)
  {
    perror("Unable to allocate memory for printer");
    return (NULL);
  }

  printer->name          = _cupsStrAlloc(name);
  printer->dnssd_name    = _cupsStrRetain(printer->name);
  printer->directory     = _cupsStrAlloc(directory);
  printer->hostname      = _cupsStrAlloc(httpGetHostname(NULL, hostname,
                                                         sizeof(hostname)));
  printer->port          = port;
  printer->state         = IPP_PRINTER_IDLE;
  printer->state_reasons = _IPP_PRINTER_NONE;
  printer->jobs          = cupsArrayNew((cups_array_func_t)compare_jobs, NULL);
  printer->next_job_id   = 1;

  _cupsRWInit(&(printer->rwlock));

 /*
  * Create the listener sockets...
  */

  if ((printer->ipv4 = create_listener(AF_INET, &(printer->port))) < 0)
  {
    perror("Unable to create IPv4 listener");
    goto bad_printer;
  }

  if ((printer->ipv6 = create_listener(AF_INET6, &(printer->port))) < 0)
  {
    perror("Unable to create IPv6 listener");
    goto bad_printer;
  }

 /*
  * Prepare values for the printer attributes...
  */

  httpAssembleURI(HTTP_URI_CODING_ALL, uri, sizeof(uri), "ipp", NULL,
		  printer->hostname, printer->port, "/ipp");
  httpAssembleURI(HTTP_URI_CODING_ALL, icons, sizeof(icons), "http", NULL,
                  printer->hostname, printer->port, "/icon.png");
  httpAssembleURI(HTTP_URI_CODING_ALL, adminurl, sizeof(adminurl), "http", NULL,
                  printer->hostname, printer->port, "/");

  snprintf(make_model, sizeof(make_model), "%s %s", make, model);

  num_formats = 1;
  formats[0]  = strdup(docformats);
  defformat   = formats[0];
  for (ptr = strchr(formats[0], ','); ptr; ptr = strchr(ptr, ','))
  {
    *ptr++ = '\0';
    formats[num_formats++] = ptr;

    if (!strcasecmp(ptr, "application/octet-stream"))
      defformat = ptr;
  }

  snprintf(device_id, sizeof(device_id), "MFG:%s;MDL:%s;", make, model);
  ptr    = device_id + strlen(device_id);
  prefix = "CMD:";
  for (i = 0; i < num_formats; i ++)
  {
    if (!strcasecmp(formats[i], "application/pdf"))
      snprintf(ptr, sizeof(device_id) - (ptr - device_id), "%sPDF", prefix);
    else if (!strcasecmp(formats[i], "application/postscript"))
      snprintf(ptr, sizeof(device_id) - (ptr - device_id), "%sPS", prefix);
    else if (!strcasecmp(formats[i], "application/vnd.hp-PCL"))
      snprintf(ptr, sizeof(device_id) - (ptr - device_id), "%sPCL", prefix);
    else if (!strcasecmp(formats[i], "image/jpeg"))
      snprintf(ptr, sizeof(device_id) - (ptr - device_id), "%sJPEG", prefix);
    else if (!strcasecmp(formats[i], "image/png"))
      snprintf(ptr, sizeof(device_id) - (ptr - device_id), "%sPNG", prefix);
    else if (strcasecmp(formats[i], "application/octet-stream"))
      snprintf(ptr, sizeof(device_id) - (ptr - device_id), "%s%s", prefix,
               formats[i]);

    ptr += strlen(ptr);
    prefix = ",";
  }
  strlcat(device_id, ";", sizeof(device_id));

 /*
  * Get the maximum spool size based on the size of the filesystem used for
  * the spool directory.  If the host OS doesn't support the statfs call
  * or the filesystem is larger than 2TiB, always report INT_MAX.
  */

#ifdef HAVE_STATFS
  if (statfs(printer->directory, &spoolinfo))
    k_supported = INT_MAX;
  else if ((spoolsize = (double)spoolinfo.f_bsize *
                        spoolinfo.f_blocks / 1024) > INT_MAX)
    k_supported = INT_MAX;
  else
    k_supported = (int)spoolsize;

#elif defined(HAVE_STATVFS)
  if (statvfs(printer->directory, &spoolinfo))
    k_supported = INT_MAX;
  else if ((spoolsize = (double)spoolinfo.f_frsize *
                        spoolinfo.f_blocks / 1024) > INT_MAX)
    k_supported = INT_MAX;
  else
    k_supported = (int)spoolsize;

#else
  k_supported = INT_MAX;
#endif /* HAVE_STATFS */

 /*
  * Create the printer attributes.  This list of attributes is sorted to improve
  * performance when the client provides a requested-attributes attribute...
  */

  printer->attrs = ippNew();

  /* charset-configured */
  ippAddString(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_CHARSET | IPP_TAG_COPY,
               "charset-configured", NULL, "utf-8");

  /* charset-supported */
  ippAddStrings(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_CHARSET | IPP_TAG_COPY,
                "charset-supported", sizeof(charsets) / sizeof(charsets[0]),
		NULL, charsets);

  /* color-supported */
  ippAddBoolean(printer->attrs, IPP_TAG_PRINTER, "color-supported",
                ppm_color > 0);

  /* compression-supported */
  ippAddString(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_KEYWORD | IPP_TAG_COPY,
	       "compression-supported", NULL, "none");

  /* copies-default */
  ippAddInteger(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_INTEGER,
                "copies-default", 1);

  /* copies-supported */
  ippAddRange(printer->attrs, IPP_TAG_PRINTER, "copies-supported", 1, 999);

  /* document-format-default */
  ippAddString(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_MIMETYPE | IPP_TAG_COPY,
               "document-format-default", NULL, "application/octet-stream");

  /* document-format-supported */
  ippAddStrings(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_MIMETYPE,
                "document-format-supported", num_formats, NULL,
		(const char * const *)formats);

  /* generated-natural-language-supported */
  ippAddString(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_LANGUAGE | IPP_TAG_COPY,
               "generated-natural-language-supported", NULL, "en");

  /* ipp-versions-supported */
  ippAddStrings(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_KEYWORD | IPP_TAG_COPY,
                "ipp-versions-supported",
		sizeof(versions) / sizeof(versions[0]), NULL, versions);

  /* job-creation-attributes-supported */
  ippAddStrings(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_KEYWORD | IPP_TAG_COPY,
                "job-creation-attributes-supported",
		sizeof(job_creation) / sizeof(job_creation[0]),
		NULL, job_creation);

  /* job-k-octets-supported */
  ippAddRange(printer->attrs, IPP_TAG_PRINTER, "job-k-octets-supported", 0,
	      k_supported);

  /* job-priority-default */
  ippAddInteger(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_INTEGER,
                "job-priority-default", 50);

  /* job-priority-supported */
  ippAddInteger(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_INTEGER,
                "job-priority-supported", 100);

  /* job-sheets-default */
  ippAddString(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_NAME | IPP_TAG_COPY,
               "job-sheets-default", NULL, "none");

  /* job-sheets-supported */
  ippAddString(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_NAME | IPP_TAG_COPY,
               "job-sheets-supported", NULL, "none");

  /* media-bottom-margin-supported */
  ippAddIntegers(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_INTEGER,
                 "media-bottom-margin-supported",
		 (int)(sizeof(media_xxx_margin_supported) /
		       sizeof(media_xxx_margin_supported[0])),
		 media_xxx_margin_supported);

  /* media-col-database */
  for (num_database = 0, i = 0;
       i < (int)(sizeof(media_col_sizes) / sizeof(media_col_sizes[0]));
       i ++)
  {
    if (media_col_sizes[i][2] == _IPP_ENV_ONLY)
      num_database += 2;		/* auto + envelope */
    else if (media_col_sizes[i][2] == _IPP_PHOTO_ONLY)
      num_database += 12;		/* auto + photographic-* + borderless */
    else
      num_database += (int)(sizeof(media_type_supported) /
                            sizeof(media_type_supported[0])) + 6;
					/* All types + borderless */
  }

  media_col_database = ippAddCollections(printer->attrs, IPP_TAG_PRINTER,
                                         "media-col-database", num_database,
					 NULL);
  for (media_col_value = media_col_database->values, i = 0;
       i < (int)(sizeof(media_col_sizes) / sizeof(media_col_sizes[0]));
       i ++)
  {
    for (j = 0;
         j < (int)(sizeof(media_type_supported) /
	           sizeof(media_type_supported[0]));
         j ++)
    {
      if (media_col_sizes[i][2] == _IPP_ENV_ONLY &&
          strcmp(media_type_supported[j], "auto") &&
	  strcmp(media_type_supported[j], "envelope"))
	continue;
      else if (media_col_sizes[i][2] == _IPP_PHOTO_ONLY &&
               strcmp(media_type_supported[j], "auto") &&
	       strncmp(media_type_supported[j], "photographic-", 13))
	continue;

      media_col_value->collection =
          create_media_col(media_supported[i], media_type_supported[j],
	                   media_col_sizes[i][0], media_col_sizes[i][1],
			   media_xxx_margin_supported[1]);
      media_col_value ++;

      if (media_col_sizes[i][2] != _IPP_ENV_ONLY &&
	  (!strcmp(media_type_supported[j], "auto") ||
	   !strncmp(media_type_supported[j], "photographic-", 13)))
      {
       /*
        * Add borderless version for this combination...
	*/

	media_col_value->collection =
	    create_media_col(media_supported[i], media_type_supported[j],
			     media_col_sizes[i][0], media_col_sizes[i][1],
			     media_xxx_margin_supported[0]);
      }
    }
  }

  /* media-col-default */
  media_col_default = create_media_col(media_supported[0],
                                       media_type_supported[0],
				       media_col_sizes[0][0],
				       media_col_sizes[0][1],
				       media_xxx_margin_supported[1]);

  ippAddCollection(printer->attrs, IPP_TAG_PRINTER, "media-col-default",
                   media_col_default);
  ippDelete(media_col_default);

  /* media-col-supported */
  ippAddStrings(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_KEYWORD | IPP_TAG_COPY,
                "media-col-supported",
		(int)(sizeof(media_col_supported) /
		      sizeof(media_col_supported[0])), NULL,
		media_col_supported);

  /* media-default */
  ippAddString(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_KEYWORD | IPP_TAG_COPY,
               "media-default", NULL, media_supported[0]);

  /* media-left-margin-supported */
  ippAddIntegers(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_INTEGER,
                 "media-left-margin-supported",
		 (int)(sizeof(media_xxx_margin_supported) /
		       sizeof(media_xxx_margin_supported[0])),
		 media_xxx_margin_supported);

  /* media-right-margin-supported */
  ippAddIntegers(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_INTEGER,
                 "media-right-margin-supported",
		 (int)(sizeof(media_xxx_margin_supported) /
		       sizeof(media_xxx_margin_supported[0])),
		 media_xxx_margin_supported);

  /* media-supported */
  ippAddStrings(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_KEYWORD | IPP_TAG_COPY,
                "media-supported",
		(int)(sizeof(media_supported) / sizeof(media_supported[0])),
		NULL, media_supported);

  /* media-top-margin-supported */
  ippAddIntegers(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_INTEGER,
                 "media-top-margin-supported",
		 (int)(sizeof(media_xxx_margin_supported) /
		       sizeof(media_xxx_margin_supported[0])),
		 media_xxx_margin_supported);

  /* multiple-document-handling-supported */
  ippAddStrings(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_KEYWORD | IPP_TAG_COPY,
                "multiple-document-handling-supported",
                sizeof(multiple_document_handling) /
		    sizeof(multiple_document_handling[0]), NULL,
	        multiple_document_handling);

  /* multiple-document-jobs-supported */
  ippAddBoolean(printer->attrs, IPP_TAG_PRINTER,
                "multiple-document-jobs-supported", 0);

  /* natural-language-configured */
  ippAddString(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_LANGUAGE | IPP_TAG_COPY,
               "natural-language-configured", NULL, "en");

  /* number-up-default */
  ippAddInteger(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_INTEGER,
                "number-up-default", 1);

  /* number-up-supported */
  ippAddInteger(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_INTEGER,
                "number-up-supported", 1);

  /* operations-supported */
  ippAddIntegers(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_ENUM,
		 "operations-supported", sizeof(ops) / sizeof(ops[0]), ops);

  /* orientation-requested-default */
  ippAddInteger(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_NOVALUE,
                "orientation-requested-default", 0);

  /* orientation-requested-supported */
  ippAddIntegers(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_ENUM,
                 "orientation-requested-supported", 4, orients);

  /* pages-per-minute */
  ippAddInteger(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_INTEGER,
                "pages-per-minute", ppm);

  /* pages-per-minute-color */
  if (ppm_color > 0)
    ippAddInteger(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_INTEGER,
                  "pages-per-minute-color", ppm_color);

  /* pdl-override-supported */
  ippAddString(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_KEYWORD | IPP_TAG_COPY,
               "pdl-override-supported", NULL, "attempted");

  /* print-quality-default */
  ippAddInteger(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_ENUM,
                "print-quality-default", IPP_QUALITY_NORMAL);

  /* print-quality-supported */
  ippAddIntegers(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_ENUM,
                 "print-quality-supported",
		 (int)(sizeof(print_quality_supported) /
		       sizeof(print_quality_supported[0])),
		 print_quality_supported);

  /* printer-device-id */
  ippAddString(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_TEXT,
	       "printer-device-id", NULL, device_id);

  /* printer-icons */
  ippAddString(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_URI,
               "printer-icons", NULL, icons);

  /* printer-is-accepting-jobs */
  ippAddBoolean(printer->attrs, IPP_TAG_PRINTER, "printer-is-accepting-jobs",
                1);

  /* printer-info */
  ippAddString(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_TEXT, "printer-info",
               NULL, name);

  /* printer-location */
  ippAddString(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_TEXT,
               "printer-location", NULL, location);

  /* printer-make-and-model */
  ippAddString(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_TEXT,
               "printer-make-and-model", NULL, make_model);

  /* printer-more-info */
  ippAddString(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_URI,
               "printer-more-info", NULL, adminurl);

  /* printer-name */
  ippAddString(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_NAME, "printer-name",
               NULL, name);

  /* printer-uri-supported */
  ippAddString(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_URI,
               "printer-uri-supported", NULL, uri);

  /* sides-default */
  ippAddString(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_KEYWORD | IPP_TAG_COPY,
               "sides-default", NULL, "one-sided");

  /* sides-supported */
  ippAddStrings(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_KEYWORD,
                "sides-supported", duplex ? 3 : 1, NULL, sides_supported);

  /* uri-authentication-supported */
  ippAddString(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_KEYWORD,
               "uri-authentication-supported", NULL, "none");

  /* uri-security-supported */
  ippAddString(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_KEYWORD,
               "uri-security-supported", NULL, "none");

  /* which-jobs-supported */
  ippAddStrings(printer->attrs, IPP_TAG_PRINTER, IPP_TAG_KEYWORD | IPP_TAG_COPY,
                "which-jobs-supported",
                sizeof(which_jobs) / sizeof(which_jobs[0]), NULL, which_jobs);

  free(formats[0]);

 /*
  * Register the printer with Bonjour...
  */

  if (!register_printer(printer, location, make, model, formats, adminurl,
                        ppm_color > 0, duplex, regtype))
    goto bad_printer;

 /*
  * Return it!
  */

  return (printer);


 /*
  * If we get here we were unable to create the printer...
  */

  bad_printer:

  delete_printer(printer);
  return (NULL);
}


/*
 * 'create_requested_array()' - Create an array for requested-attributes.
 */

static cups_array_t *			/* O - requested-attributes array */
create_requested_array(
    _ipp_client_t *client)		/* I - Client */
{
  int			i;		/* Looping var */
  ipp_attribute_t	*requested;	/* requested-attributes attribute */
  cups_array_t		*ra;		/* Requested attributes array */
  char			*value;		/* Current value */


 /*
  * Get the requested-attributes attribute, and return NULL if we don't
  * have one...
  */

  if ((requested = ippFindAttribute(client->request, "requested-attributes",
                                    IPP_TAG_KEYWORD)) == NULL)
    return (NULL);

 /*
  * If the attribute contains a single "all" keyword, return NULL...
  */

  if (requested->num_values == 1 &&
      !strcmp(requested->values[0].string.text, "all"))
    return (NULL);

 /*
  * Create an array using "strcmp" as the comparison function...
  */

  ra = cupsArrayNew((cups_array_func_t)strcmp, NULL);

  for (i = 0; i < requested->num_values; i ++)
  {
    value = requested->values[i].string.text;

    if (!strcmp(value, "job-template"))
    {
      cupsArrayAdd(ra, "copies");
      cupsArrayAdd(ra, "copies-default");
      cupsArrayAdd(ra, "copies-supported");
      cupsArrayAdd(ra, "finishings");
      cupsArrayAdd(ra, "finishings-default");
      cupsArrayAdd(ra, "finishings-supported");
      cupsArrayAdd(ra, "job-hold-until");
      cupsArrayAdd(ra, "job-hold-until-default");
      cupsArrayAdd(ra, "job-hold-until-supported");
      cupsArrayAdd(ra, "job-priority");
      cupsArrayAdd(ra, "job-priority-default");
      cupsArrayAdd(ra, "job-priority-supported");
      cupsArrayAdd(ra, "job-sheets");
      cupsArrayAdd(ra, "job-sheets-default");
      cupsArrayAdd(ra, "job-sheets-supported");
      cupsArrayAdd(ra, "media");
      cupsArrayAdd(ra, "media-col");
      cupsArrayAdd(ra, "media-col-default");
      cupsArrayAdd(ra, "media-col-supported");
      cupsArrayAdd(ra, "media-default");
      cupsArrayAdd(ra, "media-source-supported");
      cupsArrayAdd(ra, "media-supported");
      cupsArrayAdd(ra, "media-type-supported");
      cupsArrayAdd(ra, "multiple-document-handling");
      cupsArrayAdd(ra, "multiple-document-handling-default");
      cupsArrayAdd(ra, "multiple-document-handling-supported");
      cupsArrayAdd(ra, "number-up");
      cupsArrayAdd(ra, "number-up-default");
      cupsArrayAdd(ra, "number-up-supported");
      cupsArrayAdd(ra, "orientation-requested");
      cupsArrayAdd(ra, "orientation-requested-default");
      cupsArrayAdd(ra, "orientation-requested-supported");
      cupsArrayAdd(ra, "page-ranges");
      cupsArrayAdd(ra, "page-ranges-supported");
      cupsArrayAdd(ra, "printer-resolution");
      cupsArrayAdd(ra, "printer-resolution-default");
      cupsArrayAdd(ra, "printer-resolution-supported");
      cupsArrayAdd(ra, "print-quality");
      cupsArrayAdd(ra, "print-quality-default");
      cupsArrayAdd(ra, "print-quality-supported");
      cupsArrayAdd(ra, "sides");
      cupsArrayAdd(ra, "sides-default");
      cupsArrayAdd(ra, "sides-supported");
    }
    else if (!strcmp(value, "job-description"))
    {
      cupsArrayAdd(ra, "date-time-at-completed");
      cupsArrayAdd(ra, "date-time-at-creation");
      cupsArrayAdd(ra, "date-time-at-processing");
      cupsArrayAdd(ra, "job-detailed-status-message");
      cupsArrayAdd(ra, "job-document-access-errors");
      cupsArrayAdd(ra, "job-id");
      cupsArrayAdd(ra, "job-impressions");
      cupsArrayAdd(ra, "job-impressions-completed");
      cupsArrayAdd(ra, "job-k-octets");
      cupsArrayAdd(ra, "job-k-octets-processed");
      cupsArrayAdd(ra, "job-media-sheets");
      cupsArrayAdd(ra, "job-media-sheets-completed");
      cupsArrayAdd(ra, "job-message-from-operator");
      cupsArrayAdd(ra, "job-more-info");
      cupsArrayAdd(ra, "job-name");
      cupsArrayAdd(ra, "job-originating-user-name");
      cupsArrayAdd(ra, "job-printer-up-time");
      cupsArrayAdd(ra, "job-printer-uri");
      cupsArrayAdd(ra, "job-state");
      cupsArrayAdd(ra, "job-state-message");
      cupsArrayAdd(ra, "job-state-reasons");
      cupsArrayAdd(ra, "job-uri");
      cupsArrayAdd(ra, "number-of-documents");
      cupsArrayAdd(ra, "number-of-intervening-jobs");
      cupsArrayAdd(ra, "output-device-assigned");
      cupsArrayAdd(ra, "time-at-completed");
      cupsArrayAdd(ra, "time-at-creation");
      cupsArrayAdd(ra, "time-at-processing");
    }
    else if (!strcmp(value, "printer-description"))
    {
      cupsArrayAdd(ra, "charset-configured");
      cupsArrayAdd(ra, "charset-supported");
      cupsArrayAdd(ra, "color-supported");
      cupsArrayAdd(ra, "compression-supported");
      cupsArrayAdd(ra, "document-format-default");
      cupsArrayAdd(ra, "document-format-supported");
      cupsArrayAdd(ra, "generated-natural-language-supported");
      cupsArrayAdd(ra, "ipp-versions-supported");
      cupsArrayAdd(ra, "job-impressions-supported");
      cupsArrayAdd(ra, "job-k-octets-supported");
      cupsArrayAdd(ra, "job-media-sheets-supported");
      cupsArrayAdd(ra, "multiple-document-jobs-supported");
      cupsArrayAdd(ra, "multiple-operation-time-out");
      cupsArrayAdd(ra, "natural-language-configured");
      cupsArrayAdd(ra, "notify-attributes-supported");
      cupsArrayAdd(ra, "notify-lease-duration-default");
      cupsArrayAdd(ra, "notify-lease-duration-supported");
      cupsArrayAdd(ra, "notify-max-events-supported");
      cupsArrayAdd(ra, "notify-events-default");
      cupsArrayAdd(ra, "notify-events-supported");
      cupsArrayAdd(ra, "notify-pull-method-supported");
      cupsArrayAdd(ra, "notify-schemes-supported");
      cupsArrayAdd(ra, "operations-supported");
      cupsArrayAdd(ra, "pages-per-minute");
      cupsArrayAdd(ra, "pages-per-minute-color");
      cupsArrayAdd(ra, "pdl-override-supported");
      cupsArrayAdd(ra, "printer-alert");
      cupsArrayAdd(ra, "printer-alert-description");
      cupsArrayAdd(ra, "printer-current-time");
      cupsArrayAdd(ra, "printer-driver-installer");
      cupsArrayAdd(ra, "printer-info");
      cupsArrayAdd(ra, "printer-is-accepting-jobs");
      cupsArrayAdd(ra, "printer-location");
      cupsArrayAdd(ra, "printer-make-and-model");
      cupsArrayAdd(ra, "printer-message-from-operator");
      cupsArrayAdd(ra, "printer-more-info");
      cupsArrayAdd(ra, "printer-more-info-manufacturer");
      cupsArrayAdd(ra, "printer-name");
      cupsArrayAdd(ra, "printer-state");
      cupsArrayAdd(ra, "printer-state-message");
      cupsArrayAdd(ra, "printer-state-reasons");
      cupsArrayAdd(ra, "printer-up-time");
      cupsArrayAdd(ra, "printer-uri-supported");
      cupsArrayAdd(ra, "queued-job-count");
      cupsArrayAdd(ra, "reference-uri-schemes-supported");
      cupsArrayAdd(ra, "uri-authentication-supported");
      cupsArrayAdd(ra, "uri-security-supported");
    }
    else if (!strcmp(value, "printer-defaults"))
    {
      cupsArrayAdd(ra, "copies-default");
      cupsArrayAdd(ra, "document-format-default");
      cupsArrayAdd(ra, "finishings-default");
      cupsArrayAdd(ra, "job-hold-until-default");
      cupsArrayAdd(ra, "job-priority-default");
      cupsArrayAdd(ra, "job-sheets-default");
      cupsArrayAdd(ra, "media-default");
      cupsArrayAdd(ra, "media-col-default");
      cupsArrayAdd(ra, "number-up-default");
      cupsArrayAdd(ra, "orientation-requested-default");
      cupsArrayAdd(ra, "sides-default");
    }
    else if (!strcmp(value, "subscription-template"))
    {
      cupsArrayAdd(ra, "notify-attributes");
      cupsArrayAdd(ra, "notify-charset");
      cupsArrayAdd(ra, "notify-events");
      cupsArrayAdd(ra, "notify-lease-duration");
      cupsArrayAdd(ra, "notify-natural-language");
      cupsArrayAdd(ra, "notify-pull-method");
      cupsArrayAdd(ra, "notify-recipient-uri");
      cupsArrayAdd(ra, "notify-time-interval");
      cupsArrayAdd(ra, "notify-user-data");
    }
    else
      cupsArrayAdd(ra, value);
  }

  return (ra);
}


/*
 * 'delete_client()' - Close the socket and free all memory used by a client
 *                     object.
 */

static void
delete_client(_ipp_client_t *client)	/* I - Client */
{
}


/*
 * 'delete_job()' - Remove from the printer and free all memory used by a job
 *                  object.
 */

static void
delete_job(_ipp_job_t *job)		/* I - Job */
{
}


/*
 * 'delete_printer()' - Unregister, close listen sockets, and free all memory
 *                      used by a printer object.
 */

static void
delete_printer(_ipp_printer_t *printer)	/* I - Printer */
{
}


/*
 * 'dnssd_callback()' - Handle Bonjour registration events.
 */

static void
dnssd_callback(
    DNSServiceRef       sdRef,		/* I - Service reference */
    DNSServiceFlags     flags,		/* I - Status flags */
    DNSServiceErrorType errorCode,	/* I - Error, if any */
    const char          *name,		/* I - Service name */
    const char          *regtype,	/* I - Service type */
    const char          *domain,	/* I - Domain for service */
    _ipp_printer_t      *printer)	/* I - Printer */
{
}


/*
 * 'ipp_cancel_job()' - Cancel a job.
 */

static void
ipp_cancel_job(_ipp_client_t *client)	/* I - Client */
{
}


/*
 * 'ipp_create_job()' - Create a job object.
 */

static void
ipp_create_job(_ipp_client_t *client)	/* I - Client */
{
}


/*
 * 'ipp_get_job_attributes()' - Get the attributes for a job object.
 */

static void
ipp_get_job_attributes(
    _ipp_client_t *client)		/* I - Client */
{
}


/*
 * 'ipp_get_jobs()' - Get a list of job objects.
 */

static void
ipp_get_jobs(_ipp_client_t *client)	/* I - Client */
{
}


/*
 * 'ipp_get_printer_attributes()' - Get the attributes for a printer object.
 */

static void
ipp_get_printer_attributes(
    _ipp_client_t *client)		/* I - Client */
{
}


/*
 * 'ipp_print_job()' - Create a job object with an attached document.
 */

static void
ipp_print_job(_ipp_client_t *client)	/* I - Client */
{
}


/*
 * 'ipp_send_document()' - Add an attached document to a job object created with
 *                         Create-Job.
 */

static void
ipp_send_document(_ipp_client_t *client)/* I - Client */
{
}


/*
 * 'ipp_validate_job()' - Validate job creation attributes.
 */

static void
ipp_validate_job(_ipp_client_t *client)	/* I - Client */
{
}


/*
 * 'process_client()' - Process client requests on a thread.
 */

static void *				/* O - Exit status */
process_client(_ipp_client_t *client)	/* I - Client */
{
 /*
  * Loop until we are out of requests or timeout (30 seconds)...
  */

  while (httpWait(&(client->http), 30000))
    if (!process_http(client))
      break;

 /*
  * Close the conection to the client and return...
  */

  delete_client(client);

  return (NULL);
}


/*
 * 'process_http()' - Process a HTTP request.
 */

int					/* O - 1 on success, 0 on failure */
process_http(_ipp_client_t *client)	/* I - Client connection */
{
  char			line[4096],	/* Line from client... */
			operation[64],	/* Operation code from socket */
			uri[1024],	/* URI */
			version[64],	/* HTTP version number string */
			*ptr;		/* Pointer into strings */
  const char		*ext,		/* Extension */
			*type;		/* Content type */
  int			major, minor;	/* HTTP version numbers */
  int			urilen;		/* Length of URI string */
  http_status_t		status;		/* Transfer status */
  ipp_state_t		state;		/* State of IPP transfer */


 /*
  * Abort if we have an error on the connection...
  */

  if (client->http.error)
    return (0);

 /*
  * Clear state variables...
  */

  httpClearFields(&(client->http));
  ippDelete(client->request);
  ippDelete(client->response);

  client->http.activity       = time(NULL);
  client->http.version        = HTTP_1_1;
  client->http.keep_alive     = HTTP_KEEPALIVE_OFF;
  client->http.data_encoding  = HTTP_ENCODE_LENGTH;
  client->http.data_remaining = 0;
  client->request             = NULL;
  client->response            = NULL;
  client->operation           = HTTP_WAITING;

 /*
  * Read a request from the connection...
  */

  while ((ptr = httpGets(line, sizeof(line) - 1, &(client->http))) != NULL)
    if (*ptr)
      break;

  if (!ptr)
    return (0);

 /*
  * Parse the request line...
  */

  fprintf(stder, "%s %s\n", client->http.hostname, line);

  switch (sscanf(line, "%63s%1023s%63s", operation, uri, version))
  {
    case 1 :
	fprintf(stderr, "%s Bad request line.\n", client->http.hostname);
	respond_http(client, HTTP_BAD_REQUEST, NULL, 0);
	return (0);

    case 2 :
	client->http.version = HTTP_0_9;
	break;

    case 3 :
	if (sscanf(version, "HTTP/%d.%d", &major, &minor) != 2)
	{
	  fprintf(stderr, "%s Bad HTTP version.\n", client->http.hostname);
	  respond_http(client, HTTP_BAD_REQUEST, NULL, 0);
	  return (0);
	}

	if (major < 2)
	{
	  client->http.version = (http_version_t)(major * 100 + minor);
	  if (client->http.version == HTTP_1_1)
	    client->http.keep_alive = HTTP_KEEPALIVE_ON;
	  else
	    client->http.keep_alive = HTTP_KEEPALIVE_OFF;
	}
	else
	{
	  respond_http(client, HTTP_NOT_SUPPORTED, NULL, 0);
	  return (0);
	}
	break;
  }

 /*
  * Handle full URLs in the request line...
  */

  if (!strncmp(client->uri, "http:", 5) || !strncmp(client->uri, "ipp:", 4))
  {
    char	scheme[32],		/* Method/scheme */
		userpass[128],		/* Username:password */
		hostname[HTTP_MAX_HOST];/* Hostname */
    int		port;			/* Port number */

   /*
    * Separate the URI into its components...
    */

    if (httpSeparateURI(HTTP_URI_CODING_MOST, uri, scheme, sizeof(scheme),
		        userpass, sizeof(userpass),
		        hostname, sizeof(hostname), &port,
		        client->uri, sizeof(client->uri)) < HTTP_URI_OK)
    {
      fprintf(stderr, "%s Bad URI \"%s\".\n", client->http.hostname, uri);
      respond_http(client, HTTP_BAD_REQUEST, NULL, 0);
      return (0);
    }
  }
  else
  {
   /*
    * Decode URI
    */

    if (!_httpDecodeURI(client->uri, uri, sizeof(client->uri)))
    {
      fprintf(stderr, "%s Bad URI \"%s\".\n", client->http.hostname, uri);
      respond_http(client, HTTP_BAD_REQUEST, NULL, 0);
      return (0);
    }
  }

 /*
  * Process the request...
  */

  if (!strcmp(operation, "GET"))
    client->http.state = HTTP_GET;
  else if (!strcmp(operation, "POST"))
    client->http.state = HTTP_POST;
  else if (!strcmp(operation, "OPTIONS"))
    client->http.state = HTTP_OPTIONS;
  else if (!strcmp(operation, "HEAD"))
    client->http.state = HTTP_HEAD;
  else
  {
    fprintf(stderr, "%s Bad operation \"%s\".\n", client->http.hostname,
            operation);
    respond_http(client, HTTP_BAD_REQUEST, NULL, 0);
    return (0);
  }

  client->start       = time(NULL);
  client->operation   = client->http.state;
  client->http.status = HTTP_OK;

 /*
  * Parse incoming parameters until the status changes...
  */

  while ((status = httpUpdate(&(client->http))) == HTTP_CONTINUE);

  if (status != HTTP_OK)
  {
    respond_http(client, HTTP_BAD_REQUEST, NULL, 0);
    return (0);
  }

  if (!client->http.fields[HTTP_FIELD_HOST][0] &&
      client->http.version >= HTTP_1_1)
  {
   /*
    * HTTP/1.1 and higher require the "Host:" field...
    */

    respond_http(client, HTTP_BAD_REQUEST, NULL, 0);
    return (0);
  }

 /*
  * Handle HTTP Upgrade...
  */

  if (!strcasecmp(client->http.fields[HTTP_FIELD_CONNECTION], "Upgrade"))
  {
    if (!respond_http(client, HTTP_NOT_IMPLEMENTED, NULL, 0))
      return (0);
  }

 /*
  * Handle new transfers...
  */

  switch (client->operation)
  {
    case HTTP_OPTIONS :
       /*
	* Do HEAD/OPTIONS command...
	*/

	return (respond_http(client, HTTP_OK, NULL, 0));

    case HTTP_GET :
    case HTTP_HEAD :
        if (!strcmp(client->uri, "/icon.png"))
	{
	 /*
	  * Send PNG icon file.
	  */

          int		fd;		/* Icon file */
	  struct stat	fileinfo;	/* Icon file information */
	  char		buffer[4096];	/* Copy buffer */
	  ssize_t	bytes;		/* Bytes */

          if (!stat(printer->icon, &fileinfo) &&
	      (fd = open(printer->icon, O_RDONLY)) >= 0)
	  {
	    if (!respond_http(client, HTTP_OK, "image/png", fileinfo.st_size))
	    {
	      close(fd);
	      return (0);
	    }

            if (client->operation == HTTP_GET)
	    {
	      while ((bytes = read(fd, buffer, sizeof(buffer))) > 0)
		httpWrite(&(client->http), buffer, bytes);

	      httpWriteFlush(&(client->http));
	    }

	    close(fd);
	  }
	  else
	    return (respond_http(client, HTTP_NOT_FOUND, NULL, 0));
	}
	else if (strcmp(client->uri, "/"))
	  return (respond_http(client, HTTP_NOT_FOUND, NULL, 0));
	else
	  return (process_web(client));
	break;

    case HTTP_POST :
	if (client->http.data_remaining < 0 ||
	    (!client->http.fields[HTTP_FIELD_CONTENT_LENGTH][0] &&
	     client->http.data_encoding == HTTP_ENCODE_LENGTH))
	{
	 /*
	  * Negative content lengths are invalid!
	  */

	  return (respond_http(client, HTTP_BAD_REQUEST, NULL, 0));
	}

	if (strcmp(client->http.fields[HTTP_FIELD_CONTENT_TYPE],
	           "application/ipp"))
        {
         /*
          * Not an IPP POST, either update the web interface or return an
          * error.
          */

          if (client->http.expect == HTTP_CONTINUE)
	  {
	   /*
	    * Send 100-continue header...
	    */

	    if (!respond_http(client, HTTP_CONTINUE, NULL, 0))
	      return (0);
	  }

          if (EnableWeb)
            return (liteProcessWeb(con));
          else
	    return (respond_http(client, HTTP_BAD_REQUEST, NULL, 0));
	}

       /*
        * Read the IPP request...
	*/

	client->request = ippNew();

        while ((state = ippRead(&(client->http), client->request)) != IPP_DATA)
	  if (state == IPP_ERROR)
	  {
            liteLog(con, "IPP read error (%s)\n",
	            ippOpString(client->request->request.op.operation_id));

	    respond_http(client, HTTP_BAD_REQUEST, NULL, 0);
	    return (0);
	  }

	client->bytes += ippLength(client->request);

       /*
        * Now that we have the IPP request, process the request...
	*/

        return (liteProcessIPP(con));

    default :
        break; /* Anti-compiler-warning-code */
  }

  return (1);
}


/*
 * 'process_ipp()' - Process an IPP request.
 */

static int				/* O - 1 on success, 0 on error */
process_ipp(_ipp_client_t *client)	/* I - Client */
{
}



/*
 * 'register_printer()' - Register a printer object via Bonjour.
 */

static int				/* O - 1 on success, 0 on error */
register_printer(
    _ipp_printer_t *printer,		/* I - Printer */
    const char     *location,		/* I - Location */
    const char     *make,		/* I - Manufacturer */
    const char     *model,		/* I - Model name */
    const char     *formats,		/* I - Supported formats */
    const char     *adminurl,		/* I - Web interface URL */
    int            color,		/* I - 1 = color, 0 = monochrome */
    int            duplex,		/* I - 1 = duplex, 0 = simplex */
    const char     *regtype)		/* I - Service type */
{
  DNSServiceErrorType	error;		/* Error from Bonjour */
  char			make_model[256],/* Make and model together */
			product[256];	/* Product string */


 /*
  * Build the TXT record for IPP...
  */

  snprintf(make_model, sizeof(make_model), "%s %s", make, model);
  snprintf(product, sizeof(product), "(%s)", model);

  TXTRecordCreate(&(printer->ipp_txt), 1024, NULL);
  TXTRecordSetValue(&(printer->ipp_txt), "txtvers", 1, "1");
  TXTRecordSetValue(&(printer->ipp_txt), "qtotal", 1, "1");
  TXTRecordSetValue(&(printer->ipp_txt), "rp", 3, "ipp");
  TXTRecordSetValue(&(printer->ipp_txt), "ty", (uint8_t)strlen(make_model),
                    make_model);
  TXTRecordSetValue(&(printer->ipp_txt), "adminurl", (uint8_t)strlen(adminurl),
                    adminurl);
  TXTRecordSetValue(&(printer->ipp_txt), "note", (uint8_t)strlen(location),
                    location);
  TXTRecordSetValue(&(printer->ipp_txt), "priority", 1, "0");
  TXTRecordSetValue(&(printer->ipp_txt), "product", (uint8_t)strlen(product),
                    product);
  TXTRecordSetValue(&(printer->ipp_txt), "pdl", (uint8_t)strlen(formats),
                    formats);
  TXTRecordSetValue(&(printer->ipp_txt), "Color", 1, color ? "T" : "F");
  TXTRecordSetValue(&(printer->ipp_txt), "Duplex", 1, duplex ? "T" : "F");
  TXTRecordSetValue(&(printer->ipp_txt), "usb_MFG", (uint8_t)strlen(make),
                    make);
  TXTRecordSetValue(&(printer->ipp_txt), "usb_MDL", (uint8_t)strlen(model),
                    model);
  TXTRecordSetValue(&(printer->ipp_txt), "air", 4, "none");

 /*
  * Create a shared service reference for Bonjour...
  */

  if ((error = DNSServiceCreateConnection(&(printer->common_ref)))
          != kDNSServiceErr_NoError)
  {
    fprintf(stderr, "Unable to create mDNSResponder connection: %d\n", error);
    return (0);
  }

 /*
  * Register the _printer._tcp (LPD) service type with a port number of 0 to
  * defend our service name but not actually support LPD...
  */

  printer->printer_ref = printer->common_ref;

  if ((error = DNSServiceRegister(&(printer->printer_ref),
                                  kDNSServiceFlagsShareConnection,
                                  0 /* interfaceIndex */, printer->dnssd_name,
				  "_printer._tcp", NULL /* domain */,
				  NULL /* host */, 0 /* port */, 0 /* txtLen */,
				  NULL /* txtRecord */,
			          (DNSServiceRegisterReply)dnssd_callback,
			          printer)) != kDNSServiceErr_NoError)
  {
    fprintf(stderr, "Unable to register \"%s._printer._tcp\": %d\n",
            printer->dnssd_name, error);
    return (0);
  }

 /*
  * Then register the _ipp._tcp (IPP) service type with the real port number to
  * advertise our IPP printer...
  */

  printer->ipp_ref = printer->common_ref;

  if ((error = DNSServiceRegister(&(printer->ipp_ref),
                                  kDNSServiceFlagsShareConnection,
                                  0 /* interfaceIndex */, printer->dnssd_name,
				  regtype, NULL /* domain */,
				  NULL /* host */, printer->port,
				  TXTRecordGetLength(&(printer->ipp_txt)),
				  TXTRecordGetBytesPtr(&(printer->ipp_txt)),
			          (DNSServiceRegisterReply)dnssd_callback,
			          printer)) != kDNSServiceErr_NoError)
  {
    fprintf(stderr, "Unable to register \"%s.%s\": %d\n",
            printer->dnssd_name, regtype, error);
    return (0);
  }

  return (1);
}


/*
 * 'respond_http()' - Send a HTTP response.
 */

int					/* O - 1 on success, 0 on failure */
respond_http(_ipp_client_t *client,	/* I - Client */
	     http_status_t code,	/* I - HTTP status of response */
	     const char    *type,	/* I - MIME type of response */
	     size_t        length)	/* I - Length of response */
{
  char	message[1024];			/* Text message */


  fprintf(stderr, ">>>> %s\n", httpStatus(code));

  if (code == HTTP_CONTINUE)
  {
   /*
    * 100-continue doesn't send any headers...
    */

    return (httpPrintf(&(client->http), "HTTP/%d.%d 100 Continue\r\n\r\n",
		       client->http.version / 100,
		       client->http.version % 100) > 0);
  }

 /*
  * Format an error message...
  */

  if (!type && !length && code != HTTP_OK)
  {
    snprintf(message, sizeof(message), "%d - %s\n", code, httpStatus(code));

    type   = "text/plain";
    length = strlen(message);
  }
  else
    message[0] = '\0';

 /*
  * Send the HTTP status header...
  */

  httpFlushWrite(&(client->http));

  client->http.data_encoding = HTTP_ENCODE_FIELDS;

  if (httpPrintf(&(client->http), "HTTP/%d.%d %d %s\r\n", client->http.version / 100,
                 client->http.version % 100, code, httpStatus(code)) < 0)
    return (0);

 /*
  * Follow the header with the response fields...
  */

  if (httpPrintf(&(client->http), "Date: %s\r\n", httpGetDateString(time(NULL))) < 0)
    return (0);

  if (client->http.keep_alive && client->http.version >= HTTP_1_0)
  {
    if (httpPrintf(&(client->http),
                   "Connection: Keep-Alive\r\n"
                   "Keep-Alive: timeout=10\r\n") < 0)
      return (0);
  }

  if (code == HTTP_METHOD_NOT_ALLOWED || client->operation == HTTP_OPTIONS)
  {
    if (httpPrintf(&(client->http), "Allow: GET, HEAD, OPTIONS, POST\r\n") < 0)
      return (0);
  }

  if (type)
  {
    if (!strcmp(type, "text/html"))
    {
      if (httpPrintf(&(client->http),
                     "Content-Type: text/html; charset=utf-8\r\n") < 0)
	return (0);
    }
    else if (httpPrintf(&(client->http), "Content-Type: %s\r\n", type) < 0)
      return (0);
  }

  if (length == 0 && !message[0])
  {
    if (httpPrintf(&(client->http), "Transfer-Encoding: chunked\r\n\r\n") < 0)
      return (0);
  }
  else if (httpPrintf(&(client->http), "Content-Length: " CUPS_LLFMT "\r\n\r\n",
                      CUPS_LLCAST length) < 0)
    return (0);

  if (httpFlushWrite(&(client->http)) < 0)
    return (0);

 /*
  * Send the response data...
  */

  if (message[0])
  {
   /*
    * Send a plain text message.
    */

    if (httpPrintf(&(client->http), "%s", message) < 0)
    {
      DEBUG_puts("liteWriteHTTP: Unable to send plain text message.");
      return (0);
    }
  }
  else if (client->response)
  {
   /*
    * Send an IPP response...
    */

    client->http.data_encoding  = HTTP_ENCODE_LENGTH;
    client->http.data_remaining = (off_t)ippLength(client->response);

    client->response->state = IPP_IDLE;

    if (ippWrite(&(client->http), client->response) != IPP_DATA)
      return (0);
  }
  else
    client->http.data_encoding = HTTP_ENCODE_CHUNKED;

 /*
  * Flush the data and return...
  */

  return (httpFlushWrite(&(client->http)) >= 0);
}


/*
 * 'respond_ipp()' - Send an IPP response.
 */

static void
respond_ipp(_ipp_client_t *client,	/* I - Client */
            ipp_status_t  status,	/* I - status-code */
	    const char    *message,	/* I - printf-style status-message */
	    ...)			/* I - Additional args as needed */
{
}


/*
 * 'run_printer()' - Run the printer service.
 */

static void
run_printer(_ipp_printer_t *printer)	/* I - Printer */
{
  struct pollfd	polldata[3];		/* poll() data */
  int		timeout;		/* Timeout for poll() */
  _ipp_client_t	*client;		/* New client */


 /*
  * Setup poll() data for the Bonjour service socket and IPv4/6 listeners...
  */

  polldata[0].fd     = printer->ipv4;
  polldata[0].events = POLLIN;

  polldata[1].fd     = printer->ipv6;
  polldata[1].events = POLLIN;

  polldata[2].fd     = DNSServiceRefSockFD(printer->common_ref);
  polldata[2].events = POLLIN;

 /*
  * Loop until we are killed or have a hard error...
  */

  for (;;)
  {
    if (cupsArrayCount(printer->jobs))
      timeout = 10;
    else
      timeout = -1;

    if (poll(polldata, (int)(sizeof(polldata) / sizeof(polldata[0])),
             timeout) < 0)
    {
      perror("poll() failed");
      break;
    }

    if (polldata[0].revents & POLLIN)
    {
      if ((client = create_client(printer, printer->ipv4)) != NULL)
      {
	if (!_cupsThreadCreate((_cups_thread_func_t)process_client, client))
	{
	  perror("Unable to create client thread");
	  delete_client(client);
	}
      }
    }

    if (polldata[1].revents & POLLIN)
    {
      if ((client = create_client(printer, printer->ipv6)) != NULL)
      {
	if (!_cupsThreadCreate((_cups_thread_func_t)process_client, client))
	{
	  perror("Unable to create client thread");
	  delete_client(client);
	}
      }
    }

    if (polldata[2].revents & POLLIN)
      DNSServiceProcessResult(printer->common_ref);
  }
}


/*
 * 'usage()' - Show program usage.
 */

static void
usage(int status)			/* O - Exit status */
{
  if (!status)
  {
    puts(CUPS_SVERSION " - Copyright 2010 by Apple Inc. All rights reserved.");
    puts("");
  }

  puts("Usage: ippserver [options] \"name\"");
  puts("");
  puts("Options:");
  puts("-2                      Supports 2-sided printing (default=1-sided)");
  puts("-M manufacturer         Manufacturer name (default=Test)");
  printf("-d spool-directory      Spool directory "
         "(default=/tmp/ippserver.%d)\n", (int)getpid());
  puts("-f type/subtype[,...]   List of supported types "
       "(default=application/pdf,image/jpeg)");
  puts("-h                      Show program help");
  puts("-i iconfile.png         PNG icon file (default=printer.png)");
  puts("-l location             Location of printer (default=empty string)");
  puts("-m model                Model name (default=Printer)");
  puts("-p port                 Port number (default=auto)");
  puts("-r regtype              Bonjour service type (default=_ipp._tcp)");
  puts("-s speed[,color-speed]  Speed in pages per minute (default=10,0)");

  exit(status);
}


/*
 * End of "$Id$".
 */