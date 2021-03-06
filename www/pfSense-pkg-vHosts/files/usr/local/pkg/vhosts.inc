<?php
/*******************************************************************************
* Support definitions for vHosts package.
* ------------------------------------------------------------------------------
* Part of pfSense 2.3 and later (https://www.pfSense.org/).
* Copyright (c) 2004-2016 Rubicon Communications, LLC (Netgate)
*
* Inspired by vhosts package originally written by Mark Crane.
* Copyright (C) 2008 Mark J Crane
* Copyright (C) 2015 ESF, LLC
* Copyright (C) 2016 Softlife Consulting
* All rights reserved.
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************/
require_once('util.inc');
require_once('service-utils.inc');

/*******************************************************************************
* Globals
*******************************************************************************/
global $vhosts_g;
$vhosts_g = [];
$vhosts_g['subsystem_name']   = 'vhosts';
$vhosts_g['service_name']     = 'vhostsd';
$vhosts_g['conf_file_path']   = "/var/etc/{$vhosts_g['service_name']}.conf";
$vhosts_g['conf_file_prefix'] = "/var/etc/{$vhosts_g['service_name']}";
$vhosts_g['pid_file_path']    = "/var/run/{$vhosts_g['service_name']}.pid";
$vhosts_g['dirty_file_path']  = "/var/run/{$vhosts_g['subsystem_name']}.dirty";
$vhosts_g['root_base_path']   = '/usr/local/vhosts';

/*-----------------------------------------*/
/* nginx web server config file template.  */
/*                                         */
/* @servers = Server configuration blocks. */
/*-----------------------------------------*/
$vhosts_g['conf'] = <<<CONF
pid {$vhosts_g['pid_file_path']};

user  root wheel;
worker_processes  2;
error_log  syslog:server=unix:/var/run/log,facility=local5;

events
    {
    worker_connections  1024;
    }

http
    {
    include             /usr/local/etc/nginx/mime.types;
    default_type        application/octet-stream;
    add_header          X-Frame-Options SAMEORIGIN;
    server_tokens       off;
    sendfile            on;

    access_log          syslog:server=unix:/var/run/log,facility=local5 combined;
    keepalive_timeout   75;

    @servers
    }
CONF;

/*--------------------------------------------*/
/* nginx server config block template.        */
/*                                            */
/* @listen_conf = 'listen' directive(s).      */
/* @hostnames   = Host names.                 */
/* @ssl_conf    = Secure configuration block. */
/* @root_folder = Document root folder.       */
/*--------------------------------------------*/
$vhosts_g['conf_server'] = <<<CONF_SERVER

    server
        {
        @listen_conf
        server_name @hostnames;
        @ssl_conf

        client_max_body_size 200m;

        gzip on;
        gzip_types text/plain text/css text/javascript application/x-javascript text/xml application/xml application/xml+rss application/json;

        root '@root_folder';

        @custom_config

        location / { index  index.html index.htm index.php; }

        location ~ \.php$
            {
            try_files            \$uri =404; # This line closes a potential security hole
                                             # ensuring users can't execute uploaded files
                                             # see: http://forum.nginx.org/read.php?2,88845,page=3
            fastcgi_pass         unix:/var/run/php-fpm.socket;
            fastcgi_index        index.php;
            fastcgi_param        SCRIPT_FILENAME  \$document_root\$fastcgi_script_name;
            fastcgi_param        HTTP_PROXY  ""; # Fix httpoxy - https://httpoxy.org/#fix-now
            fastcgi_read_timeout 180;
            include              /usr/local/etc/nginx/fastcgi_params;
            }

        location = /favicon.ico  # Do not log requests for favicon.ico.
            {
            return               204;
            access_log           off;
            log_not_found        off;
            }
        }
CONF_SERVER;

/*-----------------------------------------*/
/* nginx server SSL config block template. */
/*                                         */
/* @crt_file_path = Certificate file path. */
/* @key_file_path = Private key file path. */
/*-----------------------------------------*/
$vhosts_g['conf_server_ssl'] = <<<CONF_SERVER_SSL

        ssl_certificate                         '@crt_file_path';
        ssl_certificate_key                     '@key_file_path';
        ssl_session_timeout                     10m;
        keepalive_timeout                       70;
        ssl_session_cache                       shared:SSL:10m;
        ssl_protocols                           TLSv1 TLSv1.1 TLSv1.2;
        ssl_ciphers                             "EECDH+AESGCM:EDH+AESGCM:AES256+EECDH:AES256+EDH";
        ssl_prefer_server_ciphers               on;
        add_header Strict-Transport-Security    "max-age=31536000";
        add_header X-Content-Type-Options       nosniff;
        ssl_session_tickets                     off;
        ssl_dhparam                             /etc/dh-parameters.4096;
CONF_SERVER_SSL;

/*******************************************************************************
* vhosts_build_service_config */
/**
* Rebuilds nginx configuration for vhosts. Called for package synchronization
* and when any vhost is modified.
*
* Config file setting: <custom_php_resync_config_command>
*******************************************************************************/
function vhosts_build_service_config() {
	global $config, $vhosts_g;

	$conf_file_path   = $vhosts_g['conf_file_path'];
	$conf_file_prefix = $vhosts_g['conf_file_prefix'];

	/*-------------------------------------*/
	/* Remove all existing configurations. */
	/*-------------------------------------*/
	conf_mount_rw();
	unlink_if_exists("{$conf_file_prefix}*");

	/*---------------------------------------*/
	/* Build 'server' section for each host. */
	/*---------------------------------------*/
	$servers = '';
	$vhosts  = $config['installedpackages']['vhosts']['config'];

	for ($i=0; $i < count($vhosts); $i++) {
		$vhost = $vhosts[$i];

		/*---------------------------*/
		/* Skip if host is disabled. */
		/*---------------------------*/
		if (isset($vhost['disabled'])) {
			continue;
		}

		/*--------------------------------------------------*/
		/* Create the root folder and a default index page. */
		/*--------------------------------------------------*/
		$root_folder = "{$vhosts_g['root_base_path']}/{$vhost['dirname']}";

		safe_mkdir($root_folder);

		if (!file_exists("$root_folder/index.php")) {
			file_put_contents("$root_folder/index.php", "<?php\necho phpinfo();\n?>\n");
		}

		/*--------------------------------------*/
		/* Build SSL config if host has a cert. */
		/*--------------------------------------*/
		unset($ssl_conf);
		if (isset($vhost['certref'])) {
			$cert = vhosts_lookup_cert($vhost['certref']);
			$crt  = base64_decode($cert['certcrt']);
			$key  = base64_decode($cert['certkey']);

			/*----------------------------------------------------------------*/
			/* Create Cert/Key files and SSL config to add to Server section. */
			/*----------------------------------------------------------------*/
			if ($crt && $key) {
				$crt_file_path = "$conf_file_prefix-$i.crt";
				$key_file_path = "$conf_file_prefix-$i.key";
				file_put_contents($crt_file_path, $crt);
				file_put_contents($key_file_path, $key);

				$ssl_conf = strtr($vhosts_g['conf_server_ssl'],
					[
					'@crt_file_path' => $crt_file_path,
					'@key_file_path' => $key_file_path
					]);
			}
		}

		/*-------------------------------------------------------*/
		/* Set listen configuration for ipaddress:port. Set      */
		/* 'server_name' to hostname and hostname:port for all   */
		/* names listed. Set to catchall if no hostnames listed. */
		/*-------------------------------------------------------*/
		$listen_conf = "listen {$vhost['ipaddress']}:{$vhost['port']}".($ssl_conf ? ' ssl;' : ';');

		if ($vhost['hostname']) {
			$hostnames = join(' ', array_map(function($n) use ($vhost) { return "$n $n:{$vhost['port']}"; }, explode(' ', $vhost['hostname'])));
		} else {
			$hostnames = '_';
		}

		/*----------------------------------------------------*/
		/* Add a semi-colon (;) to any custom configuration   */
		/* settings. nginx does not like missing semi-colons. */
		/*----------------------------------------------------*/
		$custom_config = $vhost['custom_config'];

		if (strlen(trim($custom_config)) > 0)
			if (substr(rtrim($custom_config), -1) <> ';')
				$custom_config .= ';';

		/*------------------------------------*/
		/* Add server section for this vhost. */
		/*------------------------------------*/
		$servers .= strtr($vhosts_g['conf_server'],
			[
			'@listen_conf'   => $listen_conf,
			'@hostnames'     => $hostnames,
			'@ssl_conf'      => $ssl_conf,
			'@root_folder'   => $root_folder,
			'@custom_config' => $custom_config
			]);
	}

	/*--------------------*/
	/* Build config file. */
	/*--------------------*/
	file_put_contents($conf_file_path, strtr($vhosts_g['conf'], [ '@servers' => $servers ]));
	conf_mount_ro();
}

/*******************************************************************************
* vhosts_is_dirty */
/**
* Returns true if vhosts subsystem dirty flag is set.
*
* Config file setting: <custom_php_service_status_command>
*******************************************************************************/
function vhosts_is_dirty() {
	global $vhosts_g;
	return is_subsystem_dirty($vhosts_g['subsystem_name']);
}

/*******************************************************************************
* vhosts_lookup_cert */
/**
* Returns certificate info for certificate reference ID.
*******************************************************************************/
function vhosts_lookup_cert($refid) {
	global $config;
	$certs = $config['installedpackages']['vhosts']['cert'];
	$i     = array_search($refid, array_column($certs, 'refid'));

	/*--------------------------------------*/
	/* FALSE will evaluate to 0. PHP sucks! */
	/*--------------------------------------*/
	return ($i !== FALSE ? $certs[$i] : NULL);
}

/*******************************************************************************
* vhosts_post_install */
/**
* Creates rc file and builds the service config after package install.
*
* Config file settings: <custom_php_install_command>
*******************************************************************************/
function vhosts_post_install() {
	global $vhosts_g;

	write_rcfile(
		[
		"file"  => "vhostsd.sh",
		"start" => "/usr/local/sbin/nginx -c '{$vhosts_g['conf_file_path']}' && rm -f '{$vhosts_g['dirty_file_path']}'",
		"stop"  => "kill `cat {$vhosts_g['pid_file_path']}`                  && rm -f '{$vhosts_g['dirty_file_path']}'"
		]);

	vhosts_build_service_config();
}

/*******************************************************************************
* vhosts_pre_deinstall */
/**
* Removes generated files before uninstall. 'pkg remove' command calls
* '/etc/rc.packages <name> DEINSTALL' before remove and POST-DEINSTALL after.
* <custom_php_deinstall_command> runs in POST-DEINSTALL after config file is gone.
*
* Config file setting: <custom_php_pre_deinstall_command>
*******************************************************************************/
function vhosts_pre_deinstall() {
	global $vhosts_g;
	unlink_if_exists("{$vhosts_g['conf_file_prefix']}*");
}

/*******************************************************************************
* vhosts_save_config_and_exit */
/**
* Writes out the configuration, updates the config files, and reloads page. If
* the service is running, the subsystem is marked 'dirty' and a warning message
* to restart the service is shown on the list page. 'dirty' mark is cleared when
* the service starts as declared in the <startcmd> of the config file.
*******************************************************************************/
function vhosts_save_config_and_exit() {
	global $vhosts_g;

	write_config();
	vhosts_build_service_config();

	if (vhostsd_is_running()) {
		vhosts_set_dirty(TRUE);
	}

	header("Location: ?");
	exit;
}

/*******************************************************************************
* vhosts_set_dirty */
/**
* Sets vhosts subsystem dirty flag.
*
* @param  dirty  True/false to set/clear dirty flag.
*******************************************************************************/
function vhosts_set_dirty($dirty) {
	global $vhosts_g;

	if ($dirty) {
		mark_subsystem_dirty($vhosts_g['subsystem_name']);
	} else {
		clear_subsystem_dirty($vhosts_g['subsystem_name']);
	}
}

/*******************************************************************************
* vhostsd_is_running */
/**
* Returns true if vhosts server is running.
*
* Config file setting: <custom_php_service_status_command>
*******************************************************************************/
function vhostsd_is_running() {
	global $vhosts_g;
	return is_pid_running($vhosts_g['pid_file_path']);
}
?>
