/*
 * Copyright (c) 2013 Wojtek Kaniewski <wojtekka@toxygen.net>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <dirent.h>
#include <ctype.h>
#include <errno.h>

#define TUNER_ADDR (0xc0>>1)
#define IF_ADDR (0x86>>1)

typedef enum
{
	FM_STEREO,
	FM_MONO,
	PAL_BG,
	PAL_DK,
	PAL_I,
	PAL_L, 
	PAL_LL,
} if_system_t;

static guint if_array[] =
{
	[FM_STEREO] = 10700000,
	[FM_MONO] = 10700000,
	[PAL_BG] = 38900000,
	[PAL_DK] = 38900000,
	[PAL_I] = 38900000,
	[PAL_L] = 38900000,
	[PAL_LL] = 33900000,
};

static gint af_array[] =
{
	[FM_STEREO] = 0,
	[FM_MONO] = 0,
	[PAL_BG] = 5500000,
	[PAL_DK] = 6500000,
	[PAL_I] = 6000000,
	[PAL_L] = -6500000,
	[PAL_LL] = -6500000,
};

typedef enum
{
	STEP_50_KHZ = 0,
	STEP_31_25_KHZ = 1,
	STEP_166_7_KHZ = 2,
	STEP_62_5_KHZ = 3,
} pll_step_t;

static guint pll_step_array[] =
{
	[STEP_50_KHZ] = 50000,
	[STEP_31_25_KHZ] = 31250,
	[STEP_166_7_KHZ] = 166666,
	[STEP_62_5_KHZ] = 62500,
};

static GtkAdjustment *adjustment_div;
static GtkEntry *entry_freq;
static GtkCheckButton *checkbutton_audio_carrier;
static GtkCheckButton *checkbutton_atc;
static GtkCheckButton *checkbutton_cp;
static GtkComboBox *combobox_agc;
static GtkComboBox *combobox_device;
static GtkToggleButton *togglebutton_connect;
static GtkListStore *liststore_device;
static GtkLabel *label_pll_lock;
static GtkLabel *label_agc_level;
static GtkLabel *label_video_level;
static GtkLabel *label_fm_level;
static GtkLabel *label_fm_stereo;
static GtkLabel *label_afc_level;

static gboolean update_lock = FALSE;
static int i2c_fd = -1;
static guint pll_div = 1;
static if_system_t if_system = FM_STEREO;
static guint pll_step = STEP_50_KHZ;
static gboolean audio_carrier = FALSE;
static gboolean pll_os = FALSE;
static gboolean pll_cp = TRUE;
static gboolean pll_atc = TRUE;
static guint pll_agc = 3;

int write_tuner(int fd, uint16_t div, uint8_t cb, uint8_t bb, uint8_t ab)
{
	uint8_t buf[5];

	if (ioctl(fd, I2C_SLAVE, TUNER_ADDR) == -1)
		return -1;

	buf[0] = (div >> 8) & 255;
	buf[1] = div & 255;
	buf[2] = cb;
	buf[3] = bb;
	buf[4] = ab;

	if (write(fd, buf, sizeof(buf)) != sizeof(buf))
		return -1;

	return 0;
}

int read_tuner(int fd)
{
	uint8_t buf[1];

	if (ioctl(fd, I2C_SLAVE, TUNER_ADDR) == -1)
		return -1;

	if (read(fd, buf, sizeof(buf)) != sizeof(buf))
		return -1;

	return buf[0];
}

int write_if(int fd, uint8_t b, uint8_t c, uint8_t e)
{
	uint8_t buf[4];

	if (ioctl(fd, I2C_SLAVE, IF_ADDR) == -1)
		return -1;

	buf[0] = 0;
	buf[1] = b;
	buf[2] = c;
	buf[3] = e;

	if (write(fd, buf, sizeof(buf)) != sizeof(buf))
		return -1;

	return 0;
}

int read_if(int fd)
{
	uint8_t buf[1];

	if (ioctl(fd, I2C_SLAVE, IF_ADDR) == -1)
		return -1;

	if (read(fd, buf, sizeof(buf)) != sizeof(buf))
		return -1;

	return buf[0];
}

static guint get_freq(void)
{
	return pll_step_array[pll_step] * pll_div - if_array[if_system];
}

static void update_freq(void)
{
	gchar tmp[32];
	guint freq;

	freq = get_freq();

	if (audio_carrier)
	{
		freq += af_array[if_system];
	}

	snprintf(tmp, sizeof(tmp), "%.3f MHz", (gdouble) freq / 1000000.0);
	gtk_entry_set_text(entry_freq, tmp);

	if (if_system == FM_STEREO || if_system == FM_MONO)
	{
		gtk_widget_set_sensitive(GTK_WIDGET(checkbutton_audio_carrier), FALSE);
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(checkbutton_audio_carrier), FALSE);
	}
	else
	{
		gtk_widget_set_sensitive(GTK_WIDGET(checkbutton_audio_carrier), TRUE);
	}
}

static void comm_error(void)
{
	GtkWidget *dialog;

	dialog = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "Communication error: %s", strerror(errno));
	gtk_dialog_run(GTK_DIALOG(dialog));
	gtk_widget_destroy(dialog);

	close(i2c_fd);
	i2c_fd = -1;
	gtk_toggle_button_set_active(togglebutton_connect, FALSE);
}

static void update_tuner(void)
{
	uint8_t cb, bb, ab, c, b, e;
	guint freq;

	if (i2c_fd == -1)
		return;

	freq = get_freq();

	cb = 0x80 | (pll_step << 1);

	if (pll_cp)
		cb |= 0x40;

	if (pll_os)
		cb |= 0x01;

	ab = pll_agc << 4;

	if (pll_atc)
		ab |= 0x80;

	if (if_system == FM_STEREO || if_system == FM_MONO)
	{
		bb = 0x19;
	}
	else
	{
		if (freq <= 160000000)
			bb = 0x01;
		else if (freq <= 442000000)
			bb = 0x02;
		else
			bb = 0x04;

		ab = (if_system != PAL_L && if_system != PAL_LL) ? 0xb0 : 0x40;
	}

	switch (if_system)
	{
	case FM_STEREO:	b = 0x0e; c = 0xd0; e = 0x77; break;
	case FM_MONO:	b = 0x0e; c = 0x30; e = 0x77; break;
	case PAL_BG:	b = 0x16; c = 0x70; e = 0x49; break;
	case PAL_I:	b = 0x16; c = 0x70; e = 0x4a; break;
	case PAL_DK:	b = 0x16; c = 0x70; e = 0x4b; break;
	case PAL_L:	b = 0x06; c = 0x50; e = 0x4b; break;
	case PAL_LL:	b = 0x86; c = 0x50; e = 0x53; break;
	default: return;
	}

	printf("div %d step %d if %d freq %d\ncb %02x bb %02x ab %02x b %02x c %02x e %02x\n", pll_div, pll_step_array[pll_step], if_array[if_system], freq, cb, bb, ab, b, c, e);

	if (write_tuner(i2c_fd, pll_div, cb, bb, ab) == -1)
	{
		comm_error();
		return;
	}

	if (write_if(i2c_fd, b, c, e) == -1)
	{
		comm_error();
		return;
	}
}

static void update_range(void)
{
	guint freq_lo, freq_hi;
	guint div_lo, div_hi;

	if (if_system == FM_STEREO || if_system == FM_MONO)
	{
		freq_lo = 88000000;
		freq_hi = 108000000;
	}
	else
	{
		freq_lo = 48250000;
		freq_hi = 863250000;
	}

	div_lo = (freq_lo + if_array[if_system]) / pll_step_array[pll_step];
	div_hi = (freq_hi + if_array[if_system]) / pll_step_array[pll_step];

	update_lock = TRUE;
	gtk_adjustment_set_lower(adjustment_div, div_lo);
	gtk_adjustment_set_upper(adjustment_div, div_hi);
	update_lock = FALSE;
}

void on_adjustment_div_value_changed(GtkAdjustment *widget, gpointer user_data)
{
	pll_div = (guint) gtk_adjustment_get_value(widget);

	if (!update_lock)
	{
		update_tuner();
		update_freq();
	}
}

void on_combobox_system_changed(GtkComboBox *widget, gpointer user_data)
{
	if_system = (if_system_t) gtk_combo_box_get_active(widget);

	update_lock = TRUE;
	pll_agc = (if_system == PAL_L || if_system == PAL_LL) ? 3 : 4;
	pll_atc = (if_system == PAL_L || if_system == PAL_LL);
	pll_cp = (if_system != FM_STEREO && if_system != FM_MONO);
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(combobox_agc), pll_agc);
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(checkbutton_atc), pll_atc);
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(checkbutton_cp), pll_cp);

	update_lock = FALSE;

	update_range();
	update_tuner();
	update_freq();
}

void on_combobox_step_changed(GtkComboBox *widget, gpointer user_data)
{
	guint old_pll_step;

	old_pll_step = pll_step;
	pll_step = (pll_step_t) gtk_combo_box_get_active(widget);

	update_range();

	update_lock = TRUE;
	printf("step %d->%d\n", pll_step_array[old_pll_step], pll_step_array[pll_step]);
	printf("div %d", pll_div);
	pll_div = (gdouble) pll_div * (gdouble) pll_step_array[old_pll_step] / (gdouble) pll_step_array[pll_step];
	printf("->%d\n", pll_div);
	gtk_adjustment_set_value(adjustment_div, pll_div);
	update_lock = FALSE;

	update_tuner();
	update_freq();
}

void on_combobox_agc_changed(GtkComboBox *widget, gpointer user_data)
{
	pll_agc = gtk_combo_box_get_active(widget);

	update_tuner();
}

void on_checkbutton_audio_carrier_toggled(GtkCheckButton *widget, gpointer user_data)
{
	audio_carrier = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));

	update_freq();
}

void on_checkbutton_atc_toggled(GtkCheckButton *widget, gpointer user_data)
{
	pll_atc = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));

	if (!update_lock)
	{
		update_tuner();
	}
}

void on_checkbutton_cp_toggled(GtkCheckButton *widget, gpointer user_data)
{
	pll_cp = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));

	if (!update_lock)
	{
		update_tuner();
	}
}

void on_checkbutton_os_toggled(GtkCheckButton *widget, gpointer user_data)
{
	pll_os = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));

	update_tuner();
}

void on_togglebutton_connect_toggled(GtkToggleButton *widget, gpointer user_data)
{
	if (gtk_toggle_button_get_active(widget)) {
		char *device_path;
		GtkTreeIter iter;
		GtkTreePath *path;

		path = gtk_tree_path_new_from_indices(gtk_combo_box_get_active(combobox_device), -1);
		gtk_tree_model_get_iter(GTK_TREE_MODEL(liststore_device), &iter, path);
		gtk_tree_path_free(path);

		gtk_tree_model_get(GTK_TREE_MODEL(liststore_device), &iter, 1, &device_path, -1);

		i2c_fd = open(device_path, O_RDWR);

		if (i2c_fd == -1) {
			GtkWidget *dialog;

			gtk_toggle_button_set_active(widget, FALSE);

			dialog = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "Error opening %s: %s", device_path, strerror(errno));
			gtk_dialog_run(GTK_DIALOG(dialog));
			gtk_widget_destroy(dialog);
		} else
			update_tuner();

		g_free(device_path);
	} else {
		if (i2c_fd != -1) {
			close(i2c_fd);
			i2c_fd = -1;
		}
	}
}

static gboolean handle_status_timeout(gpointer user_data)
{
	gint tuner_status;
	gint if_status;
	gchar tmp[64];
	const guint agc_values[8] = { 12, 37, 62, 87, 112, 137, 162, 187 };
	gint agc;
	const gchar *minus;

	if (i2c_fd == -1)
		return TRUE;

	if (if_system == PAL_L || if_system == PAL_LL)
	{
		tuner_status = 0;
		if_status = 0;
	}
	else
	{
		int tmp;

		tmp = read_tuner(i2c_fd);

		if (tmp == -1) {
			comm_error();
			return TRUE;
		}

		tuner_status = tmp;

		tmp = read_if(i2c_fd);

		if (tmp == -1) {
			comm_error();
			return TRUE;
		}

		if_status = tmp;
	}

	if ((tuner_status & 0x80))
		tuner_status = 0;

	if ((if_status & 0x01))
		if_status = 0;

	printf("status: tuner %02x, if %02x\n", (unsigned char) tuner_status, (unsigned char) if_status);

	gtk_label_set_markup(label_pll_lock, ((tuner_status & 0x40)) ? "<b>PLL</b>" : "PLL");
	gtk_label_set_markup(label_agc_level, ((tuner_status & 0x08)) ? "<b>AGC</b>" : "AGC");
	gtk_label_set_markup(label_video_level, ((if_status & 0x40)) ? "<b>VIDEO</b>" : "VIDEO");
	gtk_label_set_markup(label_fm_level, ((if_status & 0x20)) ? "<b>FM</b>" : "FM");
	gtk_label_set_markup(label_fm_stereo, ((tuner_status & 0x07) == 0x04) ? "<b>STEREO</b>" : "STEREO");

	agc = (if_status >> 1) & 0x0f;

	if (agc > 7) {
		agc = agc ^ 0x0f;
		minus = "-";
	} else {
		minus = "";
	}

	snprintf(tmp, sizeof(tmp), "%s%s%d.5%s", ((if_status & 0x80)) ? "<b>" : "", minus, agc_values[agc], ((if_status & 0x80)) ? "</b>" : "");
	gtk_label_set_markup(label_afc_level, tmp);

	return TRUE;
}

static gboolean enum_i2c_dev(GtkListStore *liststore)
{
	DIR *dir;
	struct dirent *de;
	guint count = 0;

	dir = opendir("/sys/class/i2c-adapter");

	if (dir == NULL)
		return FALSE;
	
	while ((de = readdir(dir)) != NULL) {
		gchar *path;
		int fd;
		gchar buf[128];
		ssize_t len;

		if (strncmp(de->d_name, "i2c-", 4) != 0)
			continue;

		path = g_strdup_printf("/sys/class/i2c-adapter/%s/name", de->d_name);
		fd = open(path, O_RDONLY);
		g_free(path);

		if (fd != -1) {
			gchar *name;

			len = read(fd, buf, sizeof(buf) - 1);

			if (len > 0) {

				while (len > 0 && isspace(buf[len - 1]))
					len--;

				buf[len] = 0;

				name = g_strdup_printf("%s (%s)", buf, de->d_name);
				path = g_strdup_printf("/dev/%s", de->d_name);
				gtk_list_store_insert_with_values(liststore, NULL, 1000, 0, name, 1, path, -1);
				g_free(path);
				g_free(name);
				count++;
			}

			close(fd);
		}
	}

	closedir(dir);

	return (count > 0);
}



int main(int argc, char **argv)
{
	GtkWidget *main_window;
	GtkBuilder *builder;

	gtk_init(&argc, &argv);

	builder = gtk_builder_new();
	gtk_builder_add_from_file(builder, "fm1216me.ui", NULL);
	main_window = GTK_WIDGET(gtk_builder_get_object(builder, "main_window"));
	adjustment_div = GTK_ADJUSTMENT(gtk_builder_get_object(builder, "adjustment_div"));
	entry_freq = GTK_ENTRY(gtk_builder_get_object(builder, "entry_freq"));
	togglebutton_connect = GTK_TOGGLE_BUTTON(gtk_builder_get_object(builder, "togglebutton_connect"));
	checkbutton_audio_carrier = GTK_CHECK_BUTTON(gtk_builder_get_object(builder, "checkbutton_audio_carrier"));
	checkbutton_atc = GTK_CHECK_BUTTON(gtk_builder_get_object(builder, "checkbutton_atc"));
	checkbutton_cp = GTK_CHECK_BUTTON(gtk_builder_get_object(builder, "checkbutton_cp"));
	combobox_agc = GTK_COMBO_BOX(gtk_builder_get_object(builder, "combobox_agc"));
	combobox_device = GTK_COMBO_BOX(gtk_builder_get_object(builder, "combobox_device"));
	label_pll_lock = GTK_LABEL(gtk_builder_get_object(builder, "label_pll_lock"));
	label_agc_level = GTK_LABEL(gtk_builder_get_object(builder, "label_agc_level"));
	label_video_level = GTK_LABEL(gtk_builder_get_object(builder, "label_video_level"));
	label_fm_level = GTK_LABEL(gtk_builder_get_object(builder, "label_fm_level"));
	label_fm_stereo = GTK_LABEL(gtk_builder_get_object(builder, "label_fm_stereo"));
	label_afc_level = GTK_LABEL(gtk_builder_get_object(builder, "label_afc_level"));
	liststore_device = GTK_LIST_STORE(gtk_builder_get_object(builder, "liststore_device"));

	if (!enum_i2c_dev(liststore_device)) {
		gtk_list_store_insert_with_values(liststore_device, NULL, 0, 0, "(No I2C adapters found)", 1, NULL, -1);
		gtk_widget_set_sensitive(GTK_WIDGET(combobox_device), FALSE);
		gtk_widget_set_sensitive(GTK_WIDGET(togglebutton_connect), FALSE);
	}

	gtk_combo_box_set_active(combobox_device, 0);

	gtk_builder_connect_signals(builder, NULL);

	g_timeout_add(500, handle_status_timeout, NULL);
	update_range();

	gtk_widget_show(main_window);                
	gtk_main();

	return 0;
}

