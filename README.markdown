Name
====

The stream_upstream_dynamic module can be modifies the upstream server without need of restarting nginx.

Table of Contents
=================
* [Name](#name)
* [Status](#status)
* [Example Configuration](#example-configuration)
* [Directives](#directives)
    * [server_resolver](#server_resolver)
* [TODO](#todo)
* [Author](#author)
* [Copyright and License](#copyright-and-license)
* [See Also](#see-also)

Status
======

This module is still under early development and is still experimental.

Example Configuration
====================
```nginx

stream {
    resolver 127.0.0.1:5353;

    upstream upstest {
        zone ups_dyn 3m;
        server_resolver;
        server www.test.com:8081;
    }

    server {
        listen       8080;
        proxy_pass   upstest;
    }
}

```

[Back to TOC](#table-of-contents)


Directives
==========

server_resolver
---------------
**syntax:** *server_resolver*

**default:** *no*

**context:** *upstream*

Specify this upstream server will be monitors changes of the IP addresses that correspond to a domain name of the server, and automatically modifies the upstream.

In order for this parameter to work, the `resolver` directive must be specified in the stream block.

[Back to TOC](#table-of-contents)


TODO
====

[Back to TOC](#table-of-contents)

Author
======

wenqiang li(vislee)

[Back to TOC](#table-of-contents)


Copyright and License
=====================

This module is licensed under the BSD license.

Copyright (C) 2018, by vislee.

All rights reserved.


[Back to TOC](#table-of-contents)

See Also
========
* http://nginx.org/en/docs/stream/ngx_stream_upstream_module.html#server

[Back to TOC](#table-of-contents)
