#include "project.h"

// defines:
//---------
#define ALLIN 1000 // uS pwm pulse for piston in
#define ALLOUT 1900 // uS pwm pulse for piston out
#define PERIOD 40000 // uS pwm period

// variables:
static struct bt_conn *default_conn = NULL;
static struct bt_gatt_ccc_cfg req_ccc_cfg[BT_GATT_CCC_MAX] = {};

struct device *ledpower; // green led (power)
struct device *ledstrip; // led strip (used as lamp)
struct device *lockpin; // lock out
struct device *gpio; // using blue led on usb board for testing
struct device *pwm_dev; // door servo driver
struct device *demoRled; // USB demo Rled
struct device *demoGled; // USB demo Gled
struct device *demoBled; // USB demo Bled
 
/*
static u8_t gbuff1[32768]; // test buffer -DEBUG ONLY-
static u8_t gbuff2[32768]; // test buffer -DEBUG ONLY-
static u8_t gbuff3[32768]; // test buffer -DEBUG ONLY-
static u8_t gbuff4[8192]; // test buffer -DEBUG ONLY-
*/

static u8_t recv_buf_static[512];
static u8_t req_buf[512];

static u8_t *recv_buf;
static u32_t b_off;
static u32_t total_len;

static u32_t start, end;
static struct in3_client *client;

/////////////////////////////
// callbacks for BT services:
//
static ssize_t read_req(struct bt_conn *conn, const struct bt_gatt_attr *attr, // callback from: BT_GATT_CHARACTERISTIC (req_char_uuid)
			void *buf, u16_t len, u16_t offset)
{
	const char *value = attr->user_data;
	u16_t value_len;

	dbg_log("<--- read req (len=%u ofs=%u)\n", len, offset);

	value_len = min(strlen(value), sizeof(req_buf)); // limit the length to req_buf size

	return bt_gatt_attr_read(conn, attr, buf, len, offset, value, value_len);
}

static ssize_t rx_data(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			 const void *buf, u16_t len, u16_t offset,
			 u8_t flags)
{
	dbg_log("<--- len:%u\n", len);
	// if processing a message don't take new ones
	if (client->msg->ready)
		{
		dbg_log("<--- write req rejected (another message being processed)!!!\n");
		return BT_GATT_ERR(BT_ATT_ERR_WRITE_REQ_REJECTED);
		}
/*  EFnote: used only for printing the MTU -no effect on the program- 
	if (total_len == 0) {
		int mtu = bt_gatt_get_mtu(conn) - 3;
		dbg_log(" GATT MTU: %d, DATA_SIZE: %d len=%d\n", mtu+3, mtu, len);
	}
*/
	if (len == 0x8) { // check if it's the header for a long transmission
		char magic[17];
		char *header = (char *) buf;
/*	EFnote: refusing the packet may hang with wrong/short packets - remove -
		if (total_len > 0) {
			dbg_log("<--- Channel already open, dismissing new request\n");
			return BT_GATT_ERR(BT_ATT_ERR_WRITE_REQ_REJECTED);
		}
*/
		sprintf(magic, "%02x%02x%02x%02x%02x%02x%02x%02x", header[0], header[1],
			header[2], header[3], header[4], header[5], header[6], header[7]);
		if (header[0] == 0x69 && // ascii "in3c"
		    header[1] == 0x6e &&
		    header[2] == 0x33 &&
		    header[3] == 0x63) {
			dbg_log("<--- Magic: 0x%s\n", magic); // a fingerprint used to identify an header packet
			if (start_message(*((int *) buf+1)))
				{
				dbg_log("<--- offset error\n");
				return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
				}
			bluetooth_clear_req();
			return len;
		}
	}

	if(recv_buf == 0) // if no space allocated
		recv_buf = k_calloc(1, total_len);
	////////////////////////////////////////////////////

	memcpy(recv_buf + b_off, buf, len);
	b_off += len;

	if (b_off >= total_len) {
		end = k_uptime_get_32();
		process_msg();
	}

	return len;
}

static void req_ccc_cfg_changed(const struct bt_gatt_attr *attr, u16_t value) // Callback from: BT_GATT_CCC (req_ccc_cfg, req_ccc_cfg_changed)
{
	dbg_log("<--- value = %u\n", value);
}

// define an UUID for msg_svc (BT_GATT_PRIMARY_SERVICE)
static struct bt_uuid_128 msg_svc_uuid = BT_UUID_INIT_128(
	0x01, 0xde, 0xbc, 0x9a, 0x78, 0x56, 0x34, 0x12,
	0x78, 0x56, 0x34, 0x12, 0x78, 0x56, 0x34, 0x12);

// define an UUID for msg_char (BT_GATT_CHARACTERISTIC #1)
static const struct bt_uuid_128 msg_char_uuid = BT_UUID_INIT_128(
	0x02, 0xde, 0xbc, 0x9a, 0x78, 0x56, 0x34, 0x12,
	0x78, 0x56, 0x34, 0x12, 0x78, 0x56, 0x34, 0x12);

// define an UUID for req_char (BT_GATT_CHARACTERISTIC #2)
static const struct bt_uuid_128 req_char_uuid = BT_UUID_INIT_128(
	0x02, 0xde, 0xbc, 0x9a, 0x78, 0x56, 0x34, 0x12,
	0x78, 0x56, 0x34, 0x12, 0x78, 0x56, 0x34, 0x13);

// define an UUID for notify (BT_GATT_CHARACTERISTIC #3)
static const struct bt_uuid_128 notify_uuid = BT_UUID_INIT_128(
	0x02, 0xde, 0xbc, 0x9a, 0x78, 0x56, 0x34, 0x12,
	0x78, 0x56, 0x34, 0x12, 0x78, 0x56, 0x34, 0x14);

static struct bt_gatt_attr msg_attrs[] = {
		BT_GATT_PRIMARY_SERVICE(&msg_svc_uuid), // [0] primary service

////////////////////////////////////////////////////////////////////////
		BT_GATT_CHARACTERISTIC(&msg_char_uuid.uuid, // [1] characteristic attribute uuid
			BT_GATT_CHRC_WRITE, // char att properties
			BT_GATT_PERM_WRITE | BT_GATT_PERM_PREPARE_WRITE, // char att permissions
			NULL, rx_data, recv_buf_static), // char att read callback, write callback, value

		BT_GATT_CHARACTERISTIC(&req_char_uuid.uuid, // [2] characteristic attribute uuid
			BT_GATT_CHRC_READ, // char att properties
			BT_GATT_PERM_READ, // char att permissions
			read_req, NULL, req_buf), // char att read callback, write callback, value

		BT_GATT_CHARACTERISTIC(&notify_uuid.uuid, // [3] characteristic attribute uuid
			BT_GATT_CHRC_NOTIFY, // char att properties
			BT_GATT_PERM_NONE, // char att permissions
			NULL, NULL, NULL), // char att read callback, write callback, value
////////////////////////////////////////////////////////////////////////

		BT_GATT_CCC(req_ccc_cfg, req_ccc_cfg_changed), // [4] initial config, callback to configuration change
};

////////
// Code:
//
int start_message(int size)
{
	u32_t old_len = total_len;
	total_len = size;
	dbg_log("RX Message (length: %lu bytes)\n", total_len);

	if (recv_buf && (old_len > total_len)) {
		memset(recv_buf, 0, total_len);
		goto out;
	}

	if (recv_buf)
		k_free(recv_buf);

	recv_buf = k_calloc(1, total_len * sizeof(char));

out:
	if (!recv_buf) {
		dbg_log("Error allocating RX buf (size=%lu)\n", total_len);
		return -1;
	}

	start = k_uptime_get_32();

	k_mutex_lock(&client->mutex, 5000);

	return 0;
}

void process_msg(void)
{
	client->msg->data = recv_buf;
	client->msg->size = total_len;

	client->msg->ready = 1;

	b_off = 0;
	total_len = 0;

	k_mutex_unlock(&client->mutex);
	in3_signal_event();
}

void clear_message(struct in3_client *c)
{
	// EFmod: in case of raw message received, msg->ready flag will NOT be reset
	//  and this may hang the program; so, do this anyway:
	dbg_log("<--- try to clear ready flag...\n");
	c->msg->ready = 0; // EFmod (check if it has been assigned previously)
	dbg_log("<--- ok\n");

	if (!recv_buf) {
		dbg_log("<--- recv_buf is empty; do not clear\n");
		return;
	}

	dbg_log("<--- try to free recv_buf...\n");
	k_free(recv_buf);
	dbg_log("<--- ok\n");

	dbg_log("<--- try to clear pointers...\n");
	c->msg->size = 0;
	c->msg->ready = 0;

	b_off = 0;
	total_len = 0;

	recv_buf = 0;
	c->msg->data = 0;
	dbg_log("<--- ok\n");
}

static struct bt_gatt_service msg_svc = BT_GATT_SERVICE(msg_attrs);

static const struct bt_data ad_discov[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
};

static void connected(struct bt_conn *conn, u8_t err)
{
	dbg_log("<---\n"); // function is shown on the left
	default_conn = conn;
}

static void disconnected(struct bt_conn *conn, u8_t reason)
{
	dbg_log("<---\n"); // function is shown on the left
}

static bool le_param_req(struct bt_conn *conn, struct bt_le_conn_param *param)
{
int mtu = bt_gatt_get_mtu(conn);
	UNUSED_VAR(mtu);

	dbg_log("<--- req MTU=%d\n", mtu);
	return true;
}

static void le_param_updated(struct bt_conn *conn, u16_t interval, u16_t latency, u16_t timeout)
{
int mtu = bt_gatt_get_mtu(conn);
	UNUSED_VAR(mtu);

	dbg_log("<--- upd MTU=%d\n", mtu);
	return;
}

static struct bt_conn_cb conn_cbs = {
	.connected = connected,
	.disconnected = disconnected,
	.le_param_req = le_param_req,
	.le_param_updated = le_param_updated,
};

static void bt_ready(int err)
{
	default_conn = NULL;
	bt_conn_cb_register(&conn_cbs);
}

void bluetooth_write_req(char *msg)
{
	k_mutex_lock(&client->mutex, 5000);

	if (strlen(msg) > sizeof(recv_buf_static)) // EFmod: may be req_buf ?
		return;

	strcpy(req_buf, msg);
	dbg_log("<--- buffer prepared for req_char:\n%s\n", req_buf);

	k_mutex_unlock(&client->mutex);

	dbg_log("<--- try to send notification for REQ...\n");
	char *send = "REQ_READY";
//  EFmod: the "normal" [3] does not work. Index [5] is out of bounds, but it works...
//	bt_gatt_notify(NULL, &msg_attrs[3], send, strlen(send));
	bt_gatt_notify(NULL, &msg_attrs[5], send, strlen(send));
	dbg_log("<--- notify sent\n");
}

void bluetooth_clear_req(void)
{
//	dbg_log("<--- try to clear req_buf...\n");
	memset(req_buf, 0, sizeof(recv_buf_static)); // EFmod: may be sizeof(req_buf) ?
//	dbg_log("<--- req_buf zeroed (# %u bytes)\n", sizeof(recv_buf_static));
}

#ifdef BT_MAC // START block: next 3 functions needed only if hardcoded BT MAC is enabled
///////////////////////////////////////////////////////////////////////////////
static int char2hex(const char *c, u8_t *x)
{
	if (*c >= '0' && *c <= '9') {
		*x = *c - '0';
	} else if (*c >= 'a' && *c <= 'f') {
		*x = *c - 'a' + 10;
	} else if (*c >= 'A' && *c <= 'F') {
		*x = *c - 'A' + 10;
	} else {
		return -EINVAL;
	}

	return 0;
}

static int str2bt_addr(const char *str, bt_addr_t *addr)
{
	int i, j;
	u8_t tmp;

	if (strlen(str) != 17) {
		return -EINVAL;
	}

	for (i = 5, j = 1; *str != '\0'; str++, j++) {
		if (!(j % 3) && (*str != ':')) {
			return -EINVAL;
		} else if (*str == ':') {
			i--;
			continue;
		}

		addr->val[i] = addr->val[i] << 4;

		if (char2hex(str, &tmp) < 0) {
			return -EINVAL;
		}

		addr->val[i] |= tmp;
	}

	return 0;
}

static int str2bt_addr_le(const char *str, const char *type, bt_addr_le_t *addr)
{
	int err;

	err = str2bt_addr(str, &addr->a);
	if (err < 0) {
		return err;
	}

	if (!strcmp(type, "public") || !strcmp(type, "(public)")) {
		addr->type = BT_ADDR_LE_PUBLIC;
	} else if (!strcmp(type, "random") || !strcmp(type, "(random)")) {
		addr->type = BT_ADDR_LE_RANDOM;
	} else {
		return -EINVAL;
	}

	return 0;
}
#endif
///////////////////////////////////////////////////////////////////// END block

static void byte_to_hex(uint8_t b, char *s)
{
	s[0] = (b / 16) + '0'; // get high nibble, add ascii '0'
	if(s[0] > '9') s[0] += 7; // convert 10..15 to 'A'..'F' (+= 39 if 'a'..'f')
	s[1] = (b & 15) + '0'; // get low nibble, add ascii '0'
	if(s[1] > '9') s[1] += 7; // convert 10..15 to 'A'..'F' (+= 39 if 'a'..'f')
	s[2] = '\0'; // null terminate
}

int bluetooth_setup(struct in3_client *c)
{
	int err, lnam, fmac;
	size_t ad_len, scan_rsp_len = 0;
	struct bt_le_adv_param param;
	const struct bt_data *ad = 0, *scan_rsp = 0;
	char deviceName[24] = "in3-"; // look in prj.conf for max length

#ifdef BT_MAC // if enabled, hardcode the BT MAC address
	bt_addr_le_t addr;
//	char *c_addr = "d3:6c:29:4b:6b:45";
	char *c_addr = "c0:00:00:00:00:00"; // EFmod: most significative 2 bits must be 1
	char *c_addr_type = "(random)";
	err = str2bt_addr_le(c_addr, c_addr_type, &addr);
	bt_set_id_addr((const bt_addr_le_t *) &addr);
#endif

	err = bt_enable(bt_ready);
	if (err) {
		dbg_log("Unable to setup bluetooth");
		return -1;
	}

	k_sleep(1000);
//	bt_dev_show_info();

	param.id = 0;
	param.interval_min = BT_GAP_ADV_FAST_INT_MIN_2;
	param.interval_max = BT_GAP_ADV_FAST_INT_MAX_2;
	param.options = (BT_LE_ADV_OPT_CONNECTABLE |
				 BT_LE_ADV_OPT_USE_NAME);
	ad = ad_discov;
	ad_len = ARRAY_SIZE(ad_discov);

	if (IS_ENABLED(CONFIG_SETTINGS)) {
		settings_load();
	}

	fmac = NRF_FICR->DEVICEADDR[0]; // get low 4 octets of BT MAC
	lnam = strlen(deviceName); // original name length (before adding the MAC)
    byte_to_hex((fmac & 0xFF000000) >> 24, deviceName +lnam); // add MAC[3]
    byte_to_hex((fmac & 0xFF0000) >> 16, deviceName +lnam +2); // add MAC[2]
    byte_to_hex((fmac & 0xFF00) >> 8, deviceName +lnam +4); // add MAC[1]
    byte_to_hex(fmac & 0xFF, deviceName +lnam +6); // add MAC[0] (byte_to_hex adds \0)
//	bt_set_name(deviceName); // BT name built with "in3-" and NORDIC (almost) unique BT MAC (es: in3-84C8C54B)
//	bt_set_name("in3-peripheral"); // EFmod: the normal one
	bt_set_name("in3-emiliotest"); // EFmod: use ONLY this for testing with Arda's App

	err = bt_le_adv_start(&param, ad, ad_len, scan_rsp, scan_rsp_len);
	if (err < 0) {
		dbg_log("Failed to start advertising (err %d)\n", err);
		return -1;
	} else {
		dbg_log("Advertising started\n");
	}

	// set client data
	client = c;
	client->msg->data = recv_buf;

	recv_buf = 0;
	b_off = 0;
	total_len = 0;

	dbg_log("Registering GATT service\n");
	err = bt_gatt_service_register(&msg_svc);

	if (err < 0) {
		dbg_log("Failed to register servic\n");
		return -1;
	} else {
		dbg_log("Service registered\n");
	}

	return 0;
}

// GPIO functions //////////////////////////////////////////////////////
void ledpower_set(int state)
{
	if(state) // if ON
		gpio_pin_write(ledpower, LEDPOWER, 0); // led with negative logic
	else // if OFF
		gpio_pin_write(ledpower, LEDPOWER, 1); // led with negative logic
}

void ledstrip_set(int state)
{
	if(state) // if ON
		gpio_pin_write(ledstrip, LEDSTRIP, 1); // led with positive logic
	else { // if OFF
		gpio_pin_write(ledstrip, LEDSTRIP, 0); // led with positive logic OFF
		gpio_pin_write(demoGled, LEDG, 1); // negative logic, green led OFF
		gpio_pin_write(demoRled, LEDR, 1); // negative logic, red led OFF
	}
}

void lock_set(int state)
{
	if(state) // if ON
		gpio_pin_write(lockpin, LOCKPIN, 1); // coil with positive logic
	else // if OFF
		gpio_pin_write(lockpin, LOCKPIN, 0); // coil with positive logic
}

void door_control(int state) // control the motorized door 
{
	if(state == 'o') { // open door
		pwm_pin_set_usec(pwm_dev, DOORPIN, PERIOD, ALLOUT); // P0.2 - period,pulse; piston All-out
		gpio_pin_write(demoGled, LEDG, 0); // negative logic, green led ON
		return;
	}
	if(state == 'c') { // close door
		pwm_pin_set_usec(pwm_dev, DOORPIN, PERIOD, ALLIN); // P0.2 - period,pulse; piston All-in
		gpio_pin_write(demoRled, LEDR, 0); // negative logic, red led ON
		return;
	}
}

int gpio_setup(void)
{
	dbg_log("Init PWM...\n");
	pwm_dev = device_get_binding("PWM_0"); // try to bind PWM_0
	if (!pwm_dev) {
		dbg_log("Cannot find PWM_0 device!\n");
	} else {
		pwm_pin_set_usec(pwm_dev, DOORPIN, 40000, 1000); // P0.2 - 40mS period - 1mS pulse; piston All-in (door closed)
	}
	
	demoRled = device_get_binding(LEDR_PORT); // Rled is P0.08
	gpio_pin_configure(demoRled, LEDR, GPIO_DIR_OUT); // set as output (see fsm.h)
	gpio_pin_write(demoRled, LEDR, 0); // negative logic, led ON

	demoGled = device_get_binding(LEDG_PORT); // Gled is P1.09
	gpio_pin_configure(demoGled, LEDG, GPIO_DIR_OUT); // set as output (see fsm.h)
	gpio_pin_write(demoGled, LEDG, 0); // negative logic, led ON

	demoBled = device_get_binding(LEDB_PORT); // Gled is P0.12
	gpio_pin_configure(demoBled, LEDB, GPIO_DIR_OUT); // set as output (see fsm.h)
	gpio_pin_write(demoBled, LEDB, 0); // negative logic, led ON

	ledpower = device_get_binding(LEDPOWER_PORT); // CONFIG_GPIO_P0_DEV_NAME
	gpio_pin_configure(ledpower, LEDPOWER, GPIO_DIR_OUT); // set as output (see fsm.h)
	gpio_pin_write(ledpower, LEDPOWER, 0); // negative logic, led ON
	
	ledstrip = device_get_binding(LEDSTRIP_PORT);
	gpio_pin_configure(ledstrip, LEDSTRIP, GPIO_DIR_OUT); // set as output (see fsm.h)
	gpio_pin_write(ledstrip, LEDSTRIP, 0); // positive logic, led stripe OFF

	lockpin = device_get_binding(LOCKPIN_PORT);
	gpio_pin_configure(lockpin, LOCKPIN, GPIO_DIR_OUT); // set as output (see fsm.h)
	gpio_pin_write(lockpin, LOCKPIN, 0); // positive logic, coil OFF

	for(int i = 0, cnt = 0; i < 5; i++, cnt++) // blink led and terminate ON
		{
		ledpower_set(cnt % 2);
		k_sleep(200);
		}
	gpio_pin_write(ledpower, LEDPOWER, 0); // negative logic, led ON
	gpio_pin_write(demoRled, LEDR, 1); // red led off
	gpio_pin_write(demoGled, LEDG, 1); // green led off
	gpio_pin_write(demoBled, LEDB, 1); // blue led off
	return 0;
}
/////////////////////// end of new GPIO functions

void main(void)
{
	dbg_log("\n\n\n\n\n***\n*** Starting in3_client...\n");
 	gpio_setup(); // setup the GPIO
/*
	memset(gbuff1, 0x55, 32768); // fill buffer with 0x55
	dbg_log("Buff1 set...\n");
	memset(gbuff2, 0x55, 32768); // fill buffer with 0x55
	dbg_log("Buff2 set...\n");
	memset(gbuff3, 0x55, 32768); // fill buffer with 0x55
	dbg_log("Buff3 set...\n");
	memset(gbuff4, 0x55, 8192); // fill buffer with 0x55
	dbg_log("Buff4 set...\n");
*/
	in3_client_start();
	return;
}