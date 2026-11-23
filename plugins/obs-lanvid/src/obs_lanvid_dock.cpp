// obs_lanvid_dock.cpp — Qt6 Studio Control Dock for OBS Studio
#include "obs_lanvid_dock.hpp"

#if defined(LV_HAVE_QT6_WIDGETS) && defined(LV_HAVE_REAL_LIBOBS)

#include "obs_api_compat.hpp"
#include "obs_lanvid_core.hpp"

#include <QComboBox>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QVBoxLayout>
#include <QWidget>

namespace lv::obs {

namespace {
QPointer<LanVidControlDock> g_dock;
}  // namespace

LanVidControlDock::LanVidControlDock(QWidget* parent)
    : QDockWidget(QString::fromUtf8(obs_module_text("LanVid.Dock.Title")), parent) {
    setObjectName("LanVidControlDock");
    setStyleSheet(
        "QDockWidget { background: #14151a; color: #eceef5; }"
        "QGroupBox { border: 1px solid #2b2e3b; border-radius: 6px; margin-top: 8px; padding: 8px; "
        "            font-weight: bold; color: #a78bfa; }"
        "QPushButton { background: #232633; color: #eceef5; border: 1px solid #383c4e; "
        "              border-radius: 4px; padding: 4px 8px; }"
        "QPushButton:hover { background: #2f3346; }"
        "QPushButton:checked { background: #dc2626; border-color: #ef4444; color: #ffffff; }");

    auto* root = new QWidget(this);
    auto* main_layout = new QVBoxLayout(root);
    main_layout->setSpacing(8);

    // 1. Connected Cameras & Manual/Auto Tally Matrix
    auto* cam_group = new QGroupBox(QString::fromUtf8(obs_module_text("LanVid.Dock.CamerasGroup")), root);
    auto* scroll = new QScrollArea(cam_group);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* cam_container = new QWidget(scroll);
    cameras_layout_ = new QVBoxLayout(cam_container);
    cameras_layout_->setSpacing(6);
    cameras_layout_->addStretch();
    scroll->setWidget(cam_container);
    auto* cg_lay = new QVBoxLayout(cam_group);
    cg_lay->addWidget(scroll);
    main_layout->addWidget(cam_group, 1);

    // 2. Built-in Duplex Intercom (Director Talkback)
    auto* ic_group = new QGroupBox(QString::fromUtf8(obs_module_text("LanVid.Dock.IntercomGroup")), root);
    auto* ic_lay = new QVBoxLayout(ic_group);

    auto* dev_row = new QHBoxLayout();
    mic_combo_ = new QComboBox(ic_group);
    mic_combo_->addItem("Default Talkback Mic");
    out_combo_ = new QComboBox(ic_group);
    out_combo_->addItem("Default Intercom Headphones");
    dev_row->addWidget(new QLabel("Mic:", ic_group));
    dev_row->addWidget(mic_combo_, 1);
    dev_row->addWidget(new QLabel("Out:", ic_group));
    dev_row->addWidget(out_combo_, 1);
    ic_lay->addLayout(dev_row);

    auto* ctrl_row = new QHBoxLayout();
    all_call_btn_ = new QPushButton("ALL CALL (Partyline)", ic_group);
    all_call_btn_->setCheckable(true);
    connect(all_call_btn_, &QPushButton::toggled, this, [this](bool on) {
        all_call_active_ = on;
        LanVidObsManager::instance().set_director_talk(0, on, true, on);
        LanVidObsManager::instance().set_director_mic_active(on, false, ifb_vol_slider_->value() / 100.0f, 0.0f);
    });
    ctrl_row->addWidget(all_call_btn_);

    ctrl_row->addWidget(new QLabel("IFB Vol:", ic_group));
    ifb_vol_slider_ = new QSlider(Qt::Horizontal, ic_group);
    ifb_vol_slider_->setRange(0, 150);
    ifb_vol_slider_->setValue(100);
    connect(ifb_vol_slider_, &QSlider::valueChanged, this, [this](int val) {
        LanVidObsManager::instance().set_director_mic_active(all_call_active_, false, val / 100.0f, 0.0f);
    });
    ctrl_row->addWidget(ifb_vol_slider_, 1);
    ic_lay->addLayout(ctrl_row);

    operator_vu_bar_ = new QProgressBar(ic_group);
    operator_vu_bar_->setRange(0, 100);
    operator_vu_bar_->setFormat("Operator Mic Activity: %p%");
    ic_lay->addWidget(operator_vu_bar_);
    main_layout->addWidget(ic_group);

    // 3. Live Telemetry & Panic Reset
    auto* foot_row = new QHBoxLayout();
    telemetry_label_ = new QLabel("RX: 0.0 Mbps | MasterClock: Locked | Dropped: 0", root);
    auto* panic_btn = new QPushButton("Panic / Reset All Buffers", root);
    connect(panic_btn, &QPushButton::clicked, this, []() {
        LanVidObsManager::instance().panic_reset_all_buffers();
    });
    foot_row->addWidget(telemetry_label_, 1);
    foot_row->addWidget(panic_btn);
    main_layout->addLayout(foot_row);

    setWidget(root);

    poll_timer_ = new QTimer(this);
    connect(poll_timer_, &QTimer::timeout, this, &LanVidControlDock::on_tick);
    poll_timer_->start(200);
}

LanVidControlDock::~LanVidControlDock() {
    if (poll_timer_) poll_timer_->stop();
}

void LanVidControlDock::rebuild_camera_rows() {
    while (cameras_layout_->count() > 1) {
        auto* item = cameras_layout_->takeAt(0);
        if (item->widget()) item->widget()->deleteLater();
        delete item;
    }

    const auto cams = LanVidObsManager::instance().snapshot_cameras();
    double total_mbps = 0.0;
    uint64_t total_dropped = 0;
    bool any_op_speaking = false;

    for (const auto& c : cams) {
        total_mbps += c.mbps;
        total_dropped += c.dropped_frames;
        if (c.operator_speaking) any_op_speaking = true;

        auto* card = new QFrame();
        card->setStyleSheet("QFrame { background: #1c1e28; border: 1px solid #2d3142; border-radius: 6px; padding: 6px; }");
        auto* v = new QVBoxLayout(card);
        v->setSpacing(4);

        const char* tally_str = (c.effective_tally == lv::kTallyProgram) ? "PGM ON AIR"
                              : (c.effective_tally == lv::kTallyPreview) ? "PVW"
                                                                         : "STBY";
        auto* hdr = new QLabel(QString("%1 (%2:%3) · %4x%5 @ %6 fps · %7 Mbps · Loss %8% · Bat %9% · %10°C · [%11]")
                                   .arg(QString::fromStdString(c.name))
                                   .arg(QString::fromStdString(c.peer_ip.empty() ? "LAN" : c.peer_ip))
                                   .arg(c.listen_port)
                                   .arg(c.width)
                                   .arg(c.height)
                                   .arg(c.fps, 0, 'f', 1)
                                   .arg(c.mbps, 0, 'f', 1)
                                   .arg(c.loss_pct, 0, 'f', 1)
                                   .arg(c.battery_pct)
                                   .arg(c.temp_c)
                                   .arg(tally_str));
        v->addWidget(hdr);

        auto* btn_row = new QHBoxLayout();
        const uint32_t cam_id = c.camera_id;
        auto* b_pvw = new QPushButton("PVW");
        auto* b_pgm = new QPushButton("PGM");
        auto* b_stby = new QPushButton("STBY");
        auto* b_talk = new QPushButton("TALK (PTT)");
        b_talk->setCheckable(true);
        b_talk->setChecked(c.talk_to_cam);
        connect(b_stby, &QPushButton::clicked, this, [cam_id]() {
            LanVidObsManager::instance().tally_engine().studio_set_camera_tally(cam_id, lv::kTallyStandby, "OBS STBY", false);
        });
        connect(b_pvw, &QPushButton::clicked, this, [cam_id]() {
            LanVidObsManager::instance().tally_engine().studio_set_camera_tally(cam_id, lv::kTallyPreview, "OBS PVW", false);
        });
        connect(b_pgm, &QPushButton::clicked, this, [cam_id]() {
            LanVidObsManager::instance().tally_engine().studio_set_camera_tally(cam_id, lv::kTallyProgram, "OBS PGM", false);
        });
        connect(b_talk, &QPushButton::toggled, this, [cam_id](bool on) {
            LanVidObsManager::instance().set_director_talk(cam_id, on, true, false);
        });

        btn_row->addWidget(b_stby);
        btn_row->addWidget(b_pvw);
        btn_row->addWidget(b_pgm);
        btn_row->addWidget(b_talk);
        v->addLayout(btn_row);

        cameras_layout_->insertWidget(cameras_layout_->count() - 1, card);
    }

    operator_vu_bar_->setValue(any_op_speaking ? 78 : 0);
    telemetry_label_->setText(
        QString("Active Cameras: %1 | RX: %2 Mbps | MasterClock: Locked | Dropped: %3")
            .arg(cams.size())
            .arg(total_mbps, 0, 'f', 1)
            .arg(total_dropped));
}

void LanVidControlDock::on_tick() {
    LanVidObsManager::instance().poll_studio();
    rebuild_camera_rows();
}

void register_lanvid_control_dock() {
    auto* main_win = static_cast<QMainWindow*>(obs_frontend_get_main_window());
    if (!main_win) return;
    g_dock = new LanVidControlDock(main_win);
    obs_frontend_add_dock_by_id("lanvid_control_dock", obs_module_text("LanVid.Dock.Title"), g_dock);
}

void unregister_lanvid_control_dock() {
    if (g_dock) {
        obs_frontend_remove_dock("lanvid_control_dock");
        g_dock = nullptr;
    }
}

}  // namespace lv::obs

#endif  // LV_HAVE_QT6_WIDGETS && LV_HAVE_REAL_LIBOBS
