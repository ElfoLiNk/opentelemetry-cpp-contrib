set -e
dnf -q -y module enable php:7.2 >/dev/null 2>&1; dnf -q -y install php httpd mod_ssl glibc-utils gdb procps-ng openssl binutils python3 libnsl >/dev/null 2>&1
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
ProxyPass /api http://127.0.0.1:8081/
ProxyPassReverse /api http://127.0.0.1:8081/
P
mkdir -p /var/www/backend && echo ok > /var/www/backend/index.html && echo '<?php echo "php ok";' > /var/www/html/t.php
httpd -M 2>/dev/null | grep -E "php|proxy_http"
httpd -t
( cd /var/www/backend && python3 -m http.server 8081 >/tmp/backend.log 2>&1 & )
sleep 2
export SEGFAULT_SIGNALS="segv abrt bus ill fpe"
mkdir -p /tmp/segv && chmod 777 /tmp/segv
traffic() { for u in / /t.php /api/ /t.php /api/; do curl -s -o /dev/null -m 10 -w "$1 curl $u=%{http_code}\n" "http://127.0.0.1$u" || echo "$1 curl $u FAILED"; done; }
segv() { echo "$1 segfaults so far: $(grep -c 'Segmentation fault' /var/log/httpd/error_log)"; }
echo "== new module: start, traffic, stop (child exit path)"
LD_PRELOAD=/lib64/libSegFault.so SEGFAULT_OUTPUT_NAME=/tmp/segv/trace.txt /usr/sbin/httpd -k start
sleep 10; traffic run; sleep 8; segv run
# attach gdb to two children that served requests, then stop
for c in $(pgrep -f "httpd -k start" | sort -n | sed -n "2,3p"); do
  gdb -q -batch -p $c -ex "handle SIGTERM nostop noprint pass" -ex "handle SIGUSR1 nostop noprint pass" -ex "handle SIGWINCH nostop noprint pass" -ex "handle SIGPIPE nostop noprint pass" -ex continue -ex "echo \\n==== BT child $c ====\\n" -ex "bt 40" > /tmp/gdb-$c.txt 2>&1 &
done
sleep 6
/usr/sbin/httpd -k graceful-stop; sleep 12; segv after-graceful-stop
sleep 3; for f in /tmp/gdb-*.txt; do echo "##### $f"; grep -vE "^\\[(New|Thread)|Reading symbols|Missing separate|Try: dnf|warning:" $f | head -60; done
grep -E "exit signal|terminating agent|Shutdown" /var/log/httpd/error_log | tail -4
echo "== libSegFault trace"; cat /tmp/segv/trace.txt 2>/dev/null | head -70
echo "== sdk.log tail"; tail -6 $N/logs/sdk.log 2>/dev/null | sed -E "s/^[0-9-]+ [0-9:.]+ UTC //"
