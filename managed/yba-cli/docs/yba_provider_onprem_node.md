## yba provider onprem node

Manage YugabyteDB Anywhere onprem node instances

### Synopsis

Manage YugabyteDB Anywhere on-premises node instances

```
yba provider onprem node [flags]
```

### Options

```
  -h, --help   help for node
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
  -n, --name string        [Optional] The name of the provider for the action. Required for create, delete, describe, update, instance-type and node.
  -o, --output string      Select the desired output format. Allowed values: table, json, pretty. (default "table")
      --timeout duration   Wait command timeout, example: 5m, 1h. (default 168h0m0s)
      --wait               Wait until the task is completed, otherwise it will exit immediately. (default true)
```

### SEE ALSO

* [yba provider onprem](yba_provider_onprem.md)	 - Manage a YugabyteDB Anywhere on-premises provider
* [yba provider onprem node add](yba_provider_onprem_node_add.md)	 - Add a node instance to YugabyteDB Anywhere on-premises provider
* [yba provider onprem node list](yba_provider_onprem_node_list.md)	 - List node instances of a YugabyteDB Anywhere on-premises provider
* [yba provider onprem node preflight](yba_provider_onprem_node_preflight.md)	 - Preflight check a node of a YugabyteDB Anywhere on-premises provider
* [yba provider onprem node remove](yba_provider_onprem_node_remove.md)	 - Delete node of a YugabyteDB Anywhere on-premises provider

