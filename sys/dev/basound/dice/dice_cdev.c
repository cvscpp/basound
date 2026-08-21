// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * dice_cdev.c — /dev/pf2626N character device for the M-Audio ProFire 2626
 *
 * Exposes the TCAT EAP router, matrix mixer and sample-clock controls to a
 * userspace mixer tool through a small ioctl interface (dice_ioctl.h).
 *
 * The ioctl handlers run synchronously over FireWire transactions, so a
 * user-triggered GET/SET can block for up to the per-transaction timeout
 * (5 s).  This matches the existing dice_read_* / dice_write_* helpers and
 * is acceptable for an infrequent control-plane operation.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/conf.h>
#include <sys/kernel.h>
#include <sys/malloc.h>

#include "dice_bsd.h"
#include "dice_ioctl.h"

static d_open_t  dice_cdev_open;
static d_close_t dice_cdev_close;
static d_ioctl_t dice_cdev_ioctl;

static struct cdevsw dice_cdevsw = {
	.d_version = D_VERSION,
	.d_open    = dice_cdev_open,
	.d_close   = dice_cdev_close,
	.d_ioctl   = dice_cdev_ioctl,
	.d_name    = "pf2626",
};

static int
dice_cdev_open(struct cdev *dev __unused, int flags __unused,
    int fmt __unused, struct thread *td __unused)
{
	return (0);
}

static int
dice_cdev_close(struct cdev *dev __unused, int flags __unused,
    int fmt __unused, struct thread *td __unused)
{
	return (0);
}

static int
dice_cdev_ioctl(struct cdev *dev, u_long cmd, caddr_t data,
    int flags __unused, struct thread *td __unused)
{
	struct dice_bsd_softc *sc = dev->si_drv1;
	int err;

	if (sc == NULL)
		return (ENXIO);

	switch (cmd) {

	case PF2626_IOCTL_GET_CONFIG: {
		struct pf2626_config *cfg = (struct pf2626_config *)data;
		uint16_t rmax, min, mout;
		uint8_t rexp, rro, mexp, mro;
		uint32_t sel, status;
		unsigned int i, m;

		err = dice_maudio_read_caps(sc, &rmax, &rexp, &rro,
					    &min, &mout, &mexp, &mro);
		if (err != 0)
			return (err);

		err = dice_maudio_get_clock(sc, &sel, &status);
		if (err != 0)
			return (err);

		memset(cfg, 0, sizeof(*cfg));
		cfg->router_max_entries = rmax;
		cfg->router_exposed = rexp;
		cfg->router_readonly = rro;
		cfg->mixer_inputs = min;
		cfg->mixer_outputs = mout;
		cfg->mixer_exposed = mexp;
		cfg->mixer_readonly = mro;
		cfg->clock_caps = sc->cfg.clock_caps;
		cfg->clock_select = sel;
		cfg->clock_status = status;

		for (i = 0; i < MAX_DICE_STREAMS; i++) {
			for (m = 0; m < SND_DICE_RATE_MODE_COUNT; m++) {
				cfg->rx_pcm_chs[i][m] = (uint16_t)
				    sc->cfg.rx_pcm_chs[i][m];
				cfg->tx_pcm_chs[i][m] = (uint16_t)
				    sc->cfg.tx_pcm_chs[i][m];
			}
			cfg->rx_midi[i] = (uint8_t)sc->cfg.rx_midi_ports[i];
			cfg->tx_midi[i] = (uint8_t)sc->cfg.tx_midi_ports[i];
		}
		return (0);
	}

	case PF2626_IOCTL_GET_ROUTER: {
		struct pf2626_router *rt = (struct pf2626_router *)data;
		unsigned int n = 0;

		if (rt->rate_mode >= SND_DICE_RATE_MODE_COUNT)
			return (EINVAL);

		err = dice_maudio_get_router(sc, rt->rate_mode,
					     rt->entries, PF2626_MAX_ROUTES, &n);
		if (err != 0)
			return (err);
		rt->count = (uint16_t)n;
		return (0);
	}

	case PF2626_IOCTL_SET_ROUTER: {
		struct pf2626_router *rt = (struct pf2626_router *)data;

		if (rt->rate_mode >= SND_DICE_RATE_MODE_COUNT ||
		    rt->count == 0 || rt->count > PF2626_MAX_ROUTES)
			return (EINVAL);

		err = dice_maudio_set_router(sc, rt->rate_mode,
					     rt->entries, rt->count);
		return (err ? err : 0);
	}

	case PF2626_IOCTL_GET_MIXER: {
		struct pf2626_mixer *mx = (struct pf2626_mixer *)data;
		uint16_t min, mout;
		uint8_t rexp __unused, rro __unused, mexp, mro;

		err = dice_maudio_read_caps(sc, NULL, NULL, NULL,
					    &min, &mout, &mexp, &mro);
		if (err != 0)
			return (err);

		if (min > PF2626_MAX_MIXER_IN)
			min = PF2626_MAX_MIXER_IN;
		if (mout > PF2626_MAX_MIXER_OUT)
			mout = PF2626_MAX_MIXER_OUT;

		memset(mx, 0, sizeof(*mx));

		err = dice_maudio_get_mixer(sc, min, mout, mx->coeff);
		if (err != 0)
			return (err);

		err = dice_maudio_get_peaks(sc, mx->input_peak, mx->output_peak);
		if (err != 0)
			return (err);

		mx->inputs = min;
		mx->outputs = mout;
		return (0);
	}

	case PF2626_IOCTL_SET_MIXER: {
		struct pf2626_mixer *mx = (struct pf2626_mixer *)data;

		if (mx->inputs == 0 || mx->inputs > PF2626_MAX_MIXER_IN ||
		    mx->outputs == 0 || mx->outputs > PF2626_MAX_MIXER_OUT)
			return (EINVAL);

		err = dice_maudio_set_mixer(sc, mx->inputs, mx->outputs,
					    mx->coeff);
		return (err ? err : 0);
	}

	case PF2626_IOCTL_GET_CLOCK: {
		struct pf2626_clock *clk = (struct pf2626_clock *)data;
		uint32_t sel, status;

		err = dice_maudio_get_clock(sc, &sel, &status);
		if (err != 0)
			return (err);

		memset(clk, 0, sizeof(*clk));
		clk->source = (uint8_t)(sel & 0xff);
		clk->rate = (uint8_t)((sel >> 8) & 0xff);
		clk->select = sel;
		clk->status = status;
		return (0);
	}

	case PF2626_IOCTL_SET_CLOCK: {
		struct pf2626_clock *clk = (struct pf2626_clock *)data;

		err = dice_maudio_set_clock(sc, clk->source, clk->rate);
		return (err ? err : 0);
	}

	default:
		return (ENOTTY);
	}
}

int
dice_cdev_create(struct dice_bsd_softc *sc, int unit)
{
	sc->cdev = make_dev(&dice_cdevsw, unit, UID_ROOT, GID_WHEEL, 0660,
	    "pf2626%d", unit);
	if (sc->cdev == NULL)
		return (ENOMEM);

	sc->cdev->si_drv1 = sc;
	device_printf(sc->dev, "mixer device: /dev/pf2626%d\n", unit);
	return (0);
}

void
dice_cdev_destroy(struct dice_bsd_softc *sc)
{
	if (sc->cdev != NULL) {
		destroy_dev(sc->cdev);
		sc->cdev = NULL;
	}
}
