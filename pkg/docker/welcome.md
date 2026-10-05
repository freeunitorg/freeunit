Welcome to FreeUnit
===================

Congratulations! FreeUnit is installed and running.

Useful Links
------------

 * https://github.com/freeunitorg/freeunit
   - Browse the source, open issues, and contribute to FreeUnit.

 * https://github.com/freeunitorg/freeunit/discussions
   - Ask questions and get help from the community.

 * https://freeunit.org
   - FreeUnit project website.

Current Configuration
---------------------
Unit's control API is currently listening for configuration changes on the Unix socket at
`/var/run/control.unit.sock` inside the container.

Read the current configuration with
```
docker exec -ti <containerID> curl --unix-socket /var/run/control.unit.sock http://localhost/config
```

---
FreeUnit - the universal web app server
