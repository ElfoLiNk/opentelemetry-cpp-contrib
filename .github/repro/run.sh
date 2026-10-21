set -e
dnf -q -y module enable php:7.2 >/dev/null 2>&1; dnf -q -y install php httpd mod_ssl gdb procps-ng openssl binutils >/dev/null 2>&1
cp -r /p/opentelemetry-webserver-sdk /opt/opentelemetry-webserver-sdk-1.29
N=/opt/opentelemetry-webserver-sdk-1.29; mkdir -p $N/logs; chmod 777 $N/logs
sed "s|__agent_log_dir__|$N/logs|g; s|/opt/opentelemetry-webserver-sdk/logs|$N/logs|g" $N/conf/opentelemetry_sdk_log4cxx.xml.template > $N/conf/opentelemetry_sdk_log4cxx.xml
grep -n "file\"" $N/conf/opentelemetry_sdk_log4cxx.xml | head -2
sed -i 's/value="info"/value="trace"/g' $N/conf/opentelemetry_sdk_log4cxx.xml
cat > /etc/httpd/conf.modules.d/99-otel.conf <<C
LoadFile $N/sdk_lib/lib/libopentelemetry_common.so
LoadFile $N/sdk_lib/lib/libopentelemetry_resources.so
LoadFile $N/sdk_lib/lib/libopentelemetry_instrumentation_scope.so
LoadFile $N/sdk_lib/lib/libopentelemetry_trace.so
LoadFile $N/sdk_lib/lib/libopentelemetry_logs.so
LoadFile $N/sdk_lib/lib/libopentelemetry_metrics.so
LoadFile $N/sdk_lib/lib/libopentelemetry_exporter_ostream_span.so
LoadFile $N/sdk_lib/lib/libopentelemetry_http_client_curl.so
LoadFile $N/sdk_lib/lib/libopentelemetry_webserver_sdk.so
LoadModule otel_apache_module $N/WebServerModule/Apache/libmod_apache_otel.so
ApacheModuleEnabled ON
ApacheModuleOtelSpanExporter otlp
ApacheModuleOtelExporterEndpoint github.com:443
ApacheModuleOtelSslEnabled ON
ApacheModuleOtelSslCertificatePath  /etc/ssl/certs/ca-bundle.crt
ApacheModuleOtelSpanProcessor Batch
ApacheModuleOtelSampler AlwaysOn
ApacheModuleOtelMaxQueueSize 1024
ApacheModuleServiceName tab-httpd
ApacheModuleServiceNamespace repro
ApacheModuleServiceInstanceId c1
ApacheModuleResolveBackends ON
ApacheModuleTraceAsError OFF
ApacheModuleWebserverContext repro tab-httpd c1
C
openssl req -x509 -newkey rsa:2048 -nodes -subj /CN=localhost -keyout /etc/pki/tls/private/localhost.key -out /etc/pki/tls/certs/localhost.crt -days 2 >/dev/null 2>&1
sed -i 's/^LoadModule mpm_event_module/#&/; s/^#LoadModule mpm_prefork_module/LoadModule mpm_prefork_module/' /etc/httpd/conf.modules.d/00-mpm.conf
httpd -V | grep MPM
cat > /etc/httpd/conf.d/zz-proxy.conf <<P
Listen 8081
<VirtualHost *:8081>
  DocumentRoot /var/www/backend
</VirtualHost>
ProxyPass /api http://127.0.0.1:8081/
ProxyPassReverse /api http://127.0.0.1:8081/
P
mkdir -p /var/www/backend && echo ok > /var/www/backend/index.html && echo '<?php echo "php ok";' > /var/www/html/t.php
httpd -M 2>/dev/null | grep -E "php|proxy_http"
httpd -t
cat > /tmp/gdb.cmd <<'G'
set pagination off
set confirm off
handle SIGPIPE nostop noprint
run -X -DFOREGROUND
echo \n==== BACKTRACE ====\n
bt 60
echo \n==== FRAME 0 ====\n
info frame
x/6i $pc
echo \n==== THREADS ====\n
info threads
thread apply all bt 15
G
( sleep 20; for u in /t.php /api/; do curl -s -o /dev/null -m 10 -w "curl $u=%{http_code}\n" "http://127.0.0.1$u" || echo "curl $u FAILED"; done; sleep 10; pkill -INT gdb || true ) &
timeout 150 gdb -q -batch -x /tmp/gdb.cmd /usr/sbin/httpd 2>&1 | grep -vE "^\[(New|Thread .* exited)|Downloading|Missing separate debuginfo|Try: dnf" | head -200
wait
echo "== sdk.log tail"; tail -8 $N/logs/sdk.log 2>/dev/null
