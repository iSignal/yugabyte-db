## yba xcluster

Manage YugabyteDB Anywhere xClusters

### Synopsis

Manage YugabyteDB Anywhere xClusters (Asynchronous Replication)

```
yba xcluster [flags]
```

### Options

```
  -h, --help   help for xcluster
```

### Options inherited from parent commands

```
  -a, --apiToken string    YugabyteDB Anywhere api token.
      --ca-cert string     CA certificate file path for secure connection to YugabyteDB Anywhere. Required when the endpoint is https and --insecure is not set.
      --config string      Full path to a specific configuration file for YBA CLI. If provided, this takes precedence over the directory specified via --directory, and the generated files are added to the same path. If not provided, the CLI will look for '.yba-cli.yaml' in the directory specified by --directory. Defaults to '$HOME/.yba-cli/.yba-cli.yaml'.
      --debug              Use debug mode, same as --logLevel debug.
      --directory string   Directory containing YBA CLI configuration and generated files. If specified, the CLI will look for a configuration file named '.yba-cli.yaml' in this directory. Defaults to '$HOME/.yba-cli/'.
      --disable-color      Disable colors in output. (default false)
  -H, --host string        YugabyteDB Anywhere Host (default "http://localhost:9000")
      --insecure           Allow insecure connections to YugabyteDB Anywhere. Value ignored for http endpoints. Defaults to false for https.
  -l, --logLevel string    Select the desired log level format. Allowed values: debug, info, warn, error, fatal. (default "info")
  -o, --output string      Select the desired output format. Allowed values: table, json, pretty. (default "table")
      --timeout duration   Wait command timeout, example: 5m, 1h. (default 168h0m0s)
      --wait               Wait until the task is completed, otherwise it will exit immediately. (default true)
```

### SEE ALSO

* [yba](yba.md)	 - yba - Command line tools to manage your YugabyteDB Anywhere (Self-managed Database-as-a-Service) resources.
* [yba xcluster create](yba_xcluster_create.md)	 - Create an asynchronous replication config in YugabyteDB Anywhere
* [yba xcluster delete](yba_xcluster_delete.md)	 - Delete a YugabyteDB Anywhere xCluster
* [yba xcluster describe](yba_xcluster_describe.md)	 - Describe a YugabyteDB Anywhere xcluster between two universes
* [yba xcluster list](yba_xcluster_list.md)	 - List a YugabyteDB Anywhere xcluster between two universes
* [yba xcluster needs-full-copy-tables](yba_xcluster_needs-full-copy-tables.md)	 - Check whether source universe tables need full copy before setting up xCluster replication
* [yba xcluster pause](yba_xcluster_pause.md)	 - Pause a YugabyteDB Anywhere xCluster
* [yba xcluster restart](yba_xcluster_restart.md)	 - Restart replication for databases in the YugabyteDB Anywhere xCluster configuration
* [yba xcluster resume](yba_xcluster_resume.md)	 - Resume a YugabyteDB Anywhere xCluster
* [yba xcluster sync](yba_xcluster_sync.md)	 - Reconcile a YugabyteDB Anywhere xcluster configuration with database
* [yba xcluster update](yba_xcluster_update.md)	 - Update an asynchronous replication config in YugabyteDB Anywhere

