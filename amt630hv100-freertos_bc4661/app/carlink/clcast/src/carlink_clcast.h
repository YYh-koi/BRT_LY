#ifndef _CARLINK_CLCAST_H_
#define _CARLINK_CLCAST_H_

#ifndef CLCAST_CONTROL_PORT
#define CLCAST_CONTROL_PORT 9000
#endif

#ifndef CLCAST_BLE_FRAME_INTERVAL_MS
#define CLCAST_BLE_FRAME_INTERVAL_MS 100
#endif

#ifndef CLCAST_VIDEO_WIDTH
#define CLCAST_VIDEO_WIDTH 640
#endif

#ifndef CLCAST_VIDEO_HEIGHT
#define CLCAST_VIDEO_HEIGHT 360
#endif

/* Template arguments are APP link, project channel, and BLE MAC. */
#ifndef CLCAST_APP_QR_TEMPLATE
#define CLCAST_APP_QR_TEMPLATE "%s?SN=%s&MAC=%s"
#endif

#ifndef CLCAST_APP_DOWNLOAD_LINK
#define CLCAST_APP_DOWNLOAD_LINK \
	"https://bucket.wealthmid.com/cycling/map/map.apk"
#endif

#ifndef CLCAST_PROJECT_CHANNEL
#define CLCAST_PROJECT_CHANNEL "TYW001"
#endif

int carlink_clcast_init(int argc, char **argv);
void carlink_clcast_enable(int enable);

#endif
