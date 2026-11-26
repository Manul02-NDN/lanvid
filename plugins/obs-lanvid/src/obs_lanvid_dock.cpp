// obs_lanvid_dock.cpp — Qt6 LanVid Studio Control Dock for OBS Studio
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
#include <QSpinBox>
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

    // 1. Connected & Discovered Cameras Matrix
    auto* cam_group = new QGroupBox(QString::fromUtf8(obs_module_text("LanVid.Dock.CamerasGroup")), root);
    auto* cg_lay = new QVBoxLayout(cam_group);

    discovered_lbl_ = new QLabel("LAN / SHM Discovery: scanning...", cam_group);
    discovered_lbl_->setWordWrap(true);
    discovered_lbl_->setStyleSheet("color: #a79ecc; font-size: 11px;");
    cg_lay->addWidget(discovered_lbl_);

    auto* scroll = new QScrollArea(cam_group);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* cam_container = new QWidget(scroll);
    cameras_layout_ = new QVBoxLayout(cam_container);
    cameras_layout_->setSpacing(6);
    cameras_layout_->addStretch();
    scroll->setWidget(cam_container);
    cg_lay->addWidget(scroll, 1);
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
    all_call_btn_ = new QPushButton("ALL CALL", ic_group);
    all_call_btn_->setCheckable(true);
    connect(all_call_btn_, &QPushButton::toggled, this, [this](bool on) {
        all_call_active_ = on;
        LanVidObsRuntime::instance().set_director_talk(0, on, true, on);
        LanVidObsRuntime::instance().set_director_mic_active(on, false, ifb_vol_slider_->value() / 100.0f, 0.0f);
    });
    ctrl_row->addWidget(all_call_btn_);

    ctrl_row->addWidget(new QLabel("IFB Vol:", ic_group));
    ifb_vol_slider_ = new QSlider(Qt::Horizontal, ic_group);
    ifb_vol_slider_->setRange(0, 150);
    ifb_vol_slider_->setValue(100);
    connect(ifb_vol_slider_, &QSlider::valueChanged, this, [this](int val) {
        LanVidObsRuntime::instance().set_director_mic_active(all_call_active_, false, val / 100.0f, 0.0f);
    });
    ctrl_row->addWidget(ifb_vol_slider_, 1);
    ic_lay->addLayout(ctrl_row);

    operator_vu_bar_ = new QProgressBar(ic_group);
    operator_vu_bar_->setRange(0, 100);
    operator_vu_bar_->setFormat("Operator Mic Activity: %p%");
    ic_lay->addWidget(operator_vu_bar_);
    main_layout->addWidget(ic_group);

    // 3. Diagnostics & Telemetry Footer
    diag_label_ = new QLabel("", root);
    diag_label_->setWordWrap(true);
    diag_label_->setStyleSheet("color: #93c5fd; font-size: 10px;");
    diag_label_->setVisible(false);
    main_layout->addWidget(diag_label_);

    auto* foot_row = new QHBoxLayout();
    telemetry_label_ = new QLabel("Active: 0 | RX: 0.0 Mbps | Dropped: 0", root);
    auto* diag_btn = new QPushButton("Network & Port Check", root);
    connect(diag_btn, &QPushButton::clicked, this, [this]() {
        LanVidObsRuntime::instance().trigger_discovery_refresh();
        const std::string rep = LanVidObsRuntime::instance().diagnose_network_and_ports();
        diag_label_->setText(QString::fromStdString(rep));
        diag_label_->setVisible(!diag_label_->isVisible());
    });
    auto* panic_btn = new QPushButton("Reset Buffers", root);
    connect(panic_btn, &QPushButton::clicked, this, []() {
        LanVidObsRuntime::instance().panic_reset_all_buffers();
    });
    foot_row->addWidget(telemetry_label_, 1);
    foot_row->addWidget(diag_btn);
    foot_row->addWidget(panic_btn);
    main_layout->addLayout(foot_row);

    setWidget(root);

    poll_timer_ = new QTimer(this);
    connect(poll_timer_, &QTimer::timeout, this, &LanVidControlDock::on_tick);
    poll_timer_->start(250);
}

LanVidControlDock::~LanVidControlDock() {
    if (poll_timer_) poll_timer_->stop();
}

void LanVidControlDock::sync_camera_rows() {
    const auto cams = LanVidObsRuntime::instance().snapshot_cameras();
    const auto disc = LanVidObsRuntime::instance().discovered_cameras();

    if (disc.empty()) {
        discovered_lbl_->setText("LAN / SHM Discovery: no remote cameras detected yet (listening on :5021/:5023/:5099 + SHM)");
    } else {
        QStringList names;
        for (const auto& d : disc) names << QString::fromStdString(d.display_name);
        discovered_lbl_->setText(QString("Discovered (%1): %2").arg(disc.size()).arg(names.join(" · ")));
    }

    bool structure_changed = (rows_.size() != cams.size());
    if (!structure_changed) {
        for (size_t i = 0; i < cams.size(); ++i) {
            if (rows_[i].uid != cams[i].uid) {
                structure_changed = true;
                break;
            }
        }
    }

    if (structure_changed) {
        for (auto& r : rows_) {
            if (r.frame) {
                cameras_layout_->removeWidget(r.frame);
                r.frame->deleteLater();
            }
        }
        rows_.clear();

        for (const auto& c : cams) {
            ActiveRowWidgets row{};
            row.uid = c.uid;
            row.camera_id = c.camera_id;
            row.frame = new QFrame();
            row.frame->setStyleSheet("QFrame { background: #1c1e28; border: 1px solid #2d3142; border-radius: 6px; padding: 6px; }");
            auto* v = new QVBoxLayout(row.frame);
            v->setSpacing(4);

            row.info_lbl = new QLabel(row.frame);
            row.info_lbl->setWordWrap(true);
            v->addWidget(row.info_lbl);

            auto* btn_row = new QHBoxLayout();
            const uint32_t cam_id = c.camera_id;

            row.badge_spin = new QSpinBox(row.frame);
            row.badge_spin->setRange(1, 16);
            row.badge_spin->setPrefix("CAM ");
            row.badge_spin->setValue(c.assigned_cam_number > 0 ? c.assigned_cam_number : 1);
            connect(row.badge_spin, QOverload<int>::of(&QSpinBox::valueChanged), this, [cam_id](int num) {
                VideoConfigSyncPayload vc{};
                vc.camera_id = cam_id;
                vc.assigned_cam_number = static_cast<uint8_t>(num);
                std::snprintf(vc.assigned_cam_tag, sizeof(vc.assigned_cam_tag), "CAM %d", num);
                vc.bitrate_kbps = 0;
                LanVidObsRuntime::instance().tally_engine().send_video_config_sync(cam_id, vc);
            });

            row.quality_combo = new QComboBox(row.frame);
            row.quality_combo->addItems({"360p", "720p", "1080p", "4K60"});
            row.quality_combo->setCurrentIndex(2);
            connect(row.quality_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [cam_id](int idx) {
                VideoConfigSyncPayload vc{};
                vc.camera_id = cam_id;
                vc.resolution_preset = static_cast<uint8_t>(idx);
                vc.target_fps = (idx == 0) ? 30 : 60;
                vc.bitrate_kbps = (idx == 0) ? 4000 : (idx == 1) ? 12000 : (idx == 2) ? 25000 : 85000;
                LanVidObsRuntime::instance().tally_engine().send_video_config_sync(cam_id, vc);
            });

            row.auto_btn = new QPushButton("AUTO", row.frame);
            row.stby_btn = new QPushButton("STBY", row.frame);
            row.pvw_btn = new QPushButton("PVW", row.frame);
            row.pgm_btn = new QPushButton("PGM", row.frame);
            row.rec_btn = new QPushButton("REC", row.frame);
            row.rec_btn->setCheckable(true);
            row.talk_btn = new QPushButton("TALK", row.frame);
            row.talk_btn->setCheckable(true);
            auto* idr_btn = new QPushButton("IDR", row.frame);

            connect(row.auto_btn, &QPushButton::clicked, this, [cam_id]() {
                LanVidObsRuntime::instance().sync_all_tallies();
                (void)cam_id;
            });
            connect(row.stby_btn, &QPushButton::clicked, this, [cam_id]() {
                LanVidObsRuntime::instance().tally_engine().studio_set_camera_tally(cam_id, lv::kTallyStandby, "OBS STBY", false);
            });
            connect(row.pvw_btn, &QPushButton::clicked, this, [cam_id]() {
                LanVidObsRuntime::instance().tally_engine().studio_set_camera_tally(cam_id, lv::kTallyPreview, "OBS PVW", false);
            });
            connect(row.pgm_btn, &QPushButton::clicked, this, [cam_id]() {
                LanVidObsRuntime::instance().tally_engine().studio_set_camera_tally(cam_id, lv::kTallyProgram, "OBS PGM", false);
            });
            connect(row.rec_btn, &QPushButton::toggled, this, [cam_id](bool on) {
                LanVidObsRuntime::instance().tally_engine().studio_set_camera_tally(cam_id, lv::kTallyProgram, "OBS REC", on);
            });
            connect(idr_btn, &QPushButton::clicked, this, [cam_id]() {
                LanVidObsRuntime::instance().tally_engine().studio_request_keyframe(cam_id);
            });
            connect(row.talk_btn, &QPushButton::toggled, this, [cam_id](bool on) {
                LanVidObsRuntime::instance().set_director_talk(cam_id, on, true, false);
            });

            btn_row->addWidget(row.badge_spin);
            btn_row->addWidget(row.quality_combo);
            btn_row->addWidget(row.auto_btn);
            btn_row->addWidget(row.stby_btn);
            btn_row->addWidget(row.pvw_btn);
            btn_row->addWidget(row.pgm_btn);
            btn_row->addWidget(row.rec_btn);
            btn_row->addWidget(idr_btn);
            btn_row->addWidget(row.talk_btn);
            v->addLayout(btn_row);

            cameras_layout_->insertWidget(cameras_layout_->count() - 1, row.frame);
            rows_.push_back(row);
        }
    }

    double total_mbps = 0.0;
    uint64_t total_dropped = 0;
    bool any_op_speaking = false;

    for (size_t i = 0; i < cams.size() && i < rows_.size(); ++i) {
        const auto& c = cams[i];
        auto& r = rows_[i];
        total_mbps += c.mbps;
        total_dropped += c.dropped_frames;
        if (c.operator_speaking) any_op_speaking = true;

        const char* tally_str = (c.effective_tally == lv::kTallyProgram) ? "PGM ON AIR"
                              : (c.effective_tally == lv::kTallyPreview) ? "PVW"
                                                                         : "STBY";
        const QString badge = QString::fromStdString(
            !c.assigned_cam_tag.empty() ? c.assigned_cam_tag : ("CAM " + std::to_string(c.assigned_cam_number)));
        const QString conflict_str = c.has_port_conflict ? " [PORT CONFLICT]" : "";
        r.info_lbl->setText(
            QString("[%1] %2 (%3:%4)%5 · %6x%7 @ %8 fps · %9 Mbps · Buf %10ms (J %11ms) · Bat %12% · %13°C · [%14]")
                .arg(badge)
                .arg(QString::fromStdString(c.name))
                .arg(QString::fromStdString(c.peer_ip.empty() ? "LAN" : c.peer_ip))
                .arg(c.listen_port)
                .arg(conflict_str)
                .arg(c.width)
                .arg(c.height)
                .arg(c.fps, 0, 'f', 1)
                .arg(c.mbps, 0, 'f', 1)
                .arg(c.playout_delay_ms, 0, 'f', 0)
                .arg(c.playout_jitter_ms, 0, 'f', 1)
                .arg(c.battery_pct)
                .arg(c.temp_c)
                .arg(tally_str));
    }

    operator_vu_bar_->setValue(any_op_speaking ? 78 : 0);
    telemetry_label_->setText(
        QString("Active: %1 | Discovered: %2 | RX: %3 Mbps | Dropped: %4")
            .arg(cams.size())
            .arg(disc.size())
            .arg(total_mbps, 0, 'f', 1)
            .arg(total_dropped));
}

void LanVidControlDock::on_tick() {
    sync_camera_rows();
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
