# mqttfs

This is a small FUSE filesystem that exposes MQTT topics from a (remote) broker
as a file tree. The content of exposed files is updated in realtime as soon as
broker sends an update via MQTT subscription.

## Building on Linux

mqttfs depends only on fuse3. Once you have it installed, just
```
make
```

## Building anywhere else

I don't care about any other platforms except Linux, so you are on your own.

## Running

Run it as any other FUSE filesystem binary providing a mountpoint. Optionally
provide host and port of your MQTT broker as environment variables. Without any
configuration it will try to connect to localhost on the standard port 1883:
```
MQTT_HOST=127.0.0.1 MQTT_PORT=1883 ./mqttfs /mount/point
```

## Usage

Anything that you can imagine. I.e. try this, in the first terminal:
```
mkdir /tmp/mqttfs
MQTT_HOST=192.168.8.3 ./mqttfs /tmp/mqttfs
cd /tmp/mqttfs
python -m http.server 8000
```

In the second terminal:
```
curl http://localhost:8000/zigbee2mqtt/bridge/state
```

## Bugs

Yes.
