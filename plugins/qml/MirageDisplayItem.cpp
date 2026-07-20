#include "MirageDisplayItem.hpp"

#include <EGL/egl.h>
#include <QByteArray>
#include <QEvent>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QPointer>
#include <QQuickWindow>
#include <QScreen>
#include <QRunnable>
#include <QSGRendererInterface>
#include <QSGSimpleTextureNode>
#include <QSGTexture>
#include <QWheelEvent>
#include <QtGui/qopenglcontext_platform.h>
#include <QtCore/qnativeinterface.h>
#include <QtQuick/qsgtexture_platform.h>
#include <algorithm>
#include <functional>
#include <limits>
#include <unistd.h>

namespace {

constexpr uint32_t fourcc(char a, char b, char c, char d) {
    return static_cast<uint32_t>(static_cast<unsigned char>(a)) |
           (static_cast<uint32_t>(static_cast<unsigned char>(b)) << 8u) |
           (static_cast<uint32_t>(static_cast<unsigned char>(c)) << 16u) |
           (static_cast<uint32_t>(static_cast<unsigned char>(d)) << 24u);
}

constexpr uint32_t DrmFormatXrgb8888 = fourcc('X', 'R', '2', '4');
constexpr uint32_t DrmFormatArgb8888 = fourcc('A', 'R', '2', '4');

constexpr uint32_t BtnLeft = 0x110u;
constexpr uint32_t BtnRight = 0x111u;
constexpr uint32_t BtnMiddle = 0x112u;
constexpr uint32_t BtnSide = 0x113u;
constexpr uint32_t BtnExtra = 0x114u;

class FunctionJob final : public QRunnable {
public:
    explicit FunctionJob(std::function<void()> function): m_function(std::move(function)) {}

    void run() override {
        if (m_function) m_function();
    }

private:
    std::function<void()> m_function;
};

uint32_t positiveU32(int value, uint32_t fallback) {
    if (value <= 0) return fallback;
    return static_cast<uint32_t>(value);
}

} // namespace

MirageDisplayItem::MirageDisplayItem(QQuickItem* parent): QQuickItem(parent) {
    setFlag(ItemHasContents, true);

    const QString runtimeDirectory = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (!runtimeDirectory.isEmpty()) {
        m_socketPath = runtimeDirectory + QStringLiteral("/mirage-wallpaper/display-v1.sock");
    }

    m_reconnectTimer.setSingleShot(true);
    m_reconnectTimer.setInterval(2000);
    connect(&m_reconnectTimer, &QTimer::timeout, this, &MirageDisplayItem::startConnection);

    m_outputUpdateTimer.setSingleShot(true);
    m_outputUpdateTimer.setInterval(25);
    connect(&m_outputUpdateTimer, &QTimer::timeout, this, &MirageDisplayItem::pushOutputUpdate);
    connect(this, &QQuickItem::windowChanged, this, &MirageDisplayItem::handleWindowChanged);
}

MirageDisplayItem::~MirageDisplayItem() {
    m_reconnectTimer.stop();
    m_outputUpdateTimer.stop();
    if (window()) window()->removeEventFilter(this);
    closeConnection();
}

void MirageDisplayItem::componentComplete() {
    QQuickItem::componentComplete();
    if (window()) handleWindowChanged(window());
}

void MirageDisplayItem::handleWindowChanged(QQuickWindow* quickWindow) {
    if (quickWindow == nullptr) return;
    quickWindow->installEventFilter(this);

    QPointer<MirageDisplayItem> guard(this);
    connect(quickWindow, &QQuickWindow::sceneGraphInitialized, this, [guard]() {
        if (guard) guard->initializeRenderer();
    }, Qt::DirectConnection);
    connect(quickWindow, &QQuickWindow::afterRendering, this, [guard]() {
        if (guard) guard->releaseAfterRendering();
    }, Qt::DirectConnection);
    connect(quickWindow, &QQuickWindow::sceneGraphInvalidated, this, [guard]() {
        if (guard) guard->invalidateRenderer();
    }, Qt::DirectConnection);

    if (quickWindow->isSceneGraphInitialized()) {
        quickWindow->scheduleRenderJob(new FunctionJob([guard]() {
            if (guard) guard->initializeRenderer();
        }), QQuickWindow::BeforeSynchronizingStage);
        quickWindow->update();
    }
}

void MirageDisplayItem::initializeRenderer() {
    if (m_rendererReady.load()) return;

    QOpenGLContext* context = QOpenGLContext::currentContext();
    if (context == nullptr || window() == nullptr || window()->rendererInterface() == nullptr ||
        window()->rendererInterface()->graphicsApi() != QSGRendererInterface::OpenGL) {
        return;
    }

    auto* eglContext = context->nativeInterface<QNativeInterface::QEGLContext>();
    if (eglContext == nullptr || eglContext->display() == EGL_NO_DISPLAY) return;

    md_egl_context_t importerContext {
        .display = eglContext->display(),
        .get_proc_address = nullptr,
    };
    m_importer = md_egl_importer_new(&importerContext);
    if (m_importer == nullptr) return;

    m_imageTargetTexture = reinterpret_cast<GlEglImageTargetTexture2D>(
        eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    if (m_imageTargetTexture == nullptr) {
        md_egl_importer_free(m_importer);
        m_importer = nullptr;
        return;
    }

    m_rendererReady.store(true);
    QMetaObject::invokeMethod(this, &MirageDisplayItem::startConnection, Qt::QueuedConnection);
}

void MirageDisplayItem::invalidateRenderer() {
    if (!m_rendererReady.exchange(false) && m_importer == nullptr) return;

    uint64_t releaseGeneration = 0;
    bool finishRelease = false;
    {
        QMutexLocker locker(&m_stateMutex);
        releaseGeneration = m_releaseGeneration;
        finishRelease = m_releaseNeedsFinish;
        m_releaseGeneration = 0;
        m_releaseNeedsFinish = false;
    }

    releaseRenderPool();
    md_egl_importer_free(m_importer);
    m_importer = nullptr;
    m_imageTargetTexture = nullptr;

    if (finishRelease && releaseGeneration != 0) {
        QMetaObject::invokeMethod(this, [this, releaseGeneration]() {
            finishDeferredUnbind(static_cast<qulonglong>(releaseGeneration));
        }, Qt::QueuedConnection);
    }
    QMetaObject::invokeMethod(this, &MirageDisplayItem::closeConnection, Qt::QueuedConnection);
}

void MirageDisplayItem::setSocketPath(const QString& value) {
    if (m_socketPath == value) return;
    m_socketPath = value;
    emit socketPathChanged();
    closeConnection();
    scheduleReconnect();
}

void MirageDisplayItem::setOutputStableId(const QString& value) {
    if (m_outputStableId == value) return;
    m_outputStableId = value;
    emit outputChanged();
    m_outputUpdateTimer.start();
}

void MirageDisplayItem::setOutputName(const QString& value) {
    if (m_outputName == value) return;
    m_outputName = value;
    emit outputChanged();
    m_outputUpdateTimer.start();
}

void MirageDisplayItem::setPhysicalWidth(int value) {
    value = std::max(value, 1);
    if (m_physicalWidth == value) return;
    m_physicalWidth = value;
    emit outputChanged();
    m_outputUpdateTimer.start();
}

void MirageDisplayItem::setPhysicalHeight(int value) {
    value = std::max(value, 1);
    if (m_physicalHeight == value) return;
    m_physicalHeight = value;
    emit outputChanged();
    m_outputUpdateTimer.start();
}

void MirageDisplayItem::setLogicalWidth(int value) {
    value = std::max(value, 1);
    if (m_logicalWidth == value) return;
    m_logicalWidth = value;
    emit outputChanged();
    m_outputUpdateTimer.start();
}

void MirageDisplayItem::setLogicalHeight(int value) {
    value = std::max(value, 1);
    if (m_logicalHeight == value) return;
    m_logicalHeight = value;
    emit outputChanged();
    m_outputUpdateTimer.start();
}

void MirageDisplayItem::setScale120(int value) {
    value = std::max(value, 1);
    if (m_scale120 == value) return;
    m_scale120 = value;
    emit outputChanged();
    m_outputUpdateTimer.start();
}

void MirageDisplayItem::setRefreshMhz(int value) {
    value = std::max(value, 1);
    if (m_refreshMhz == value) return;
    m_refreshMhz = value;
    emit outputChanged();
    m_outputUpdateTimer.start();
}

void MirageDisplayItem::setOutputTransform(OutputTransform value) {
    if (m_outputTransform == value) return;
    m_outputTransform = value;
    emit outputChanged();
    m_outputUpdateTimer.start();
}

void MirageDisplayItem::setPointerForwarding(bool value) {
    if (m_pointerForwarding == value) return;
    m_pointerForwarding = value;
    emit pointerForwardingChanged();
}

void MirageDisplayItem::setWindowStateFlags(quint32 value) {
    if (m_windowStateFlags == value) return;
    m_windowStateFlags = value;
    emit windowStateFlagsChanged();
    if (m_display != nullptr && md_display_connection_state(m_display) == MD_CONNECTION_READY) {
        (void)md_display_send_window_state(m_display, static_cast<uint32_t>(value));
        armWritable();
    }
}

md_output_info_t MirageDisplayItem::makeOutputInfo(QByteArray& stableId, QByteArray& name) const {
    stableId = m_outputStableId.trimmed().toUtf8();
    name = m_outputName.trimmed().toUtf8();
    if (stableId.isEmpty()) stableId = QByteArrayLiteral("kde:unknown");
    if (name.isEmpty()) name = QByteArrayLiteral("KDE wallpaper");

    uint32_t refreshMhz = positiveU32(m_refreshMhz, 60000);
    if (window() != nullptr && window()->screen() != nullptr &&
        window()->screen()->refreshRate() > 0.0) {
        const qreal screenRefresh = window()->screen()->refreshRate() * 1000.0;
        if (screenRefresh > 0.0 && screenRefresh < static_cast<qreal>(std::numeric_limits<uint32_t>::max())) {
            refreshMhz = static_cast<uint32_t>(screenRefresh);
        }
    }

    return md_output_info_t {
        .stable_id = stableId.constData(),
        .name = name.constData(),
        .physical_width = positiveU32(m_physicalWidth, 1),
        .physical_height = positiveU32(m_physicalHeight, 1),
        .logical_width = positiveU32(m_logicalWidth, 1),
        .logical_height = positiveU32(m_logicalHeight, 1),
        .scale_120 = positiveU32(m_scale120, 120),
        .refresh_mhz = refreshMhz,
        .transform = static_cast<md_transform_t>(m_outputTransform),
        .drm_render_major = 0,
        .drm_render_minor = 0,
        .input_caps = MD_INPUT_POINTER_ENTER_LEAVE | MD_INPUT_POINTER_MOTION |
                      MD_INPUT_POINTER_BUTTON | MD_INPUT_POINTER_AXIS |
                      MD_INPUT_NON_CONSUMING,
    };
}

void MirageDisplayItem::startConnection() {
    if (!isComponentComplete() || !m_rendererReady.load() || m_display != nullptr ||
        m_socketPath.isEmpty()) {
        return;
    }

    md_display_callbacks_t callbacks {
        .on_connected = &MirageDisplayItem::onConnected,
        .on_buffers_ready = &MirageDisplayItem::onBuffersReady,
        .on_buffers_releasing = &MirageDisplayItem::onBuffersReleasing,
        .on_config = &MirageDisplayItem::onConfig,
        .on_frame = &MirageDisplayItem::onFrame,
        .on_disconnected = &MirageDisplayItem::onDisconnected,
        .user_data = this,
    };
    m_display = md_display_new(&callbacks);
    if (m_display == nullptr) {
        scheduleReconnect();
        return;
    }

    const md_format_cap_t formats[] {
        {.fourcc = DrmFormatXrgb8888, .plane_count = 1, .modifier = 0},
        {.fourcc = DrmFormatArgb8888, .plane_count = 1, .modifier = 0},
    };
    md_consumer_caps_t capabilities {
        .features = MD_FEATURE_EXPLICIT_SYNC | MD_FEATURE_POINTER_AXIS |
                    MD_FEATURE_WINDOW_STATE,
        .sync_caps = 1,
        .color_caps = 0,
        .max_width = 16384,
        .max_height = 16384,
        .device_uuid = {},
        .driver_uuid = {},
        .formats = formats,
        .format_count = static_cast<uint32_t>(std::size(formats)),
    };
    QByteArray stableId;
    QByteArray outputNameBytes;
    md_output_info_t output = makeOutputInfo(stableId, outputNameBytes);
    const QByteArray socketBytes = m_socketPath.toUtf8();

    int result = md_display_begin_connect(m_display, socketBytes.constData(),
                                          "mirage-plasma", "0.1.0",
                                          &output, &capabilities);
    if (result != MD_OK) {
        closeConnection();
        scheduleReconnect();
        return;
    }

    int fd = md_display_get_fd(m_display);
    if (fd < 0) {
        closeConnection();
        scheduleReconnect();
        return;
    }
    m_readNotifier = new QSocketNotifier(fd, QSocketNotifier::Read, this);
    m_writeNotifier = new QSocketNotifier(fd, QSocketNotifier::Write, this);
    connect(m_readNotifier, &QSocketNotifier::activated,
            this, &MirageDisplayItem::advanceHandshake);
    connect(m_writeNotifier, &QSocketNotifier::activated,
            this, &MirageDisplayItem::advanceHandshake);
    advanceHandshake();
}

void MirageDisplayItem::advanceHandshake() {
    if (m_display == nullptr) return;
    for (int iteration = 0; iteration < 16; ++iteration) {
        int result = md_display_advance_handshake(m_display);
        if (result == MD_HANDSHAKE_PROGRESS) continue;
        if (result == MD_HANDSHAKE_DONE) {
            disconnect(m_readNotifier, nullptr, this, nullptr);
            disconnect(m_writeNotifier, nullptr, this, nullptr);
            connect(m_readNotifier, &QSocketNotifier::activated,
                    this, &MirageDisplayItem::dispatchSocket);
            connect(m_writeNotifier, &QSocketNotifier::activated,
                    this, &MirageDisplayItem::flushSocket);
            m_readNotifier->setEnabled(true);
            m_writeNotifier->setEnabled(false);
            (void)md_display_send_window_state(m_display,
                                                static_cast<uint32_t>(m_windowStateFlags));
            armWritable();
            return;
        }
        if (result == MD_HANDSHAKE_NEED_READ || result == MD_HANDSHAKE_NEED_WRITE) {
            m_readNotifier->setEnabled(result == MD_HANDSHAKE_NEED_READ);
            m_writeNotifier->setEnabled(result == MD_HANDSHAKE_NEED_WRITE);
            return;
        }
        handleConnectionFailure();
        return;
    }
    handleConnectionFailure();
}

void MirageDisplayItem::dispatchSocket() {
    if (m_display == nullptr) return;
    int result = md_display_dispatch(m_display);
    if (result < 0) {
        handleConnectionFailure();
        return;
    }
    armWritable();
}

void MirageDisplayItem::flushSocket() {
    if (m_display == nullptr) return;
    if (md_display_handle_writable(m_display) < 0) {
        handleConnectionFailure();
        return;
    }
    armWritable();
}

void MirageDisplayItem::armWritable() {
    if (m_writeNotifier != nullptr && m_display != nullptr) {
        m_writeNotifier->setEnabled(md_display_wants_writable(m_display));
    }
}

void MirageDisplayItem::pushOutputUpdate() {
    if (m_display == nullptr || md_display_connection_state(m_display) != MD_CONNECTION_READY) {
        return;
    }
    QByteArray stableId;
    QByteArray outputNameBytes;
    md_output_info_t output = makeOutputInfo(stableId, outputNameBytes);
    if (md_display_update_output(m_display, &output) != MD_OK) return;
    armWritable();
}

void MirageDisplayItem::finishDeferredUnbind(qulonglong generation) {
    if (m_display == nullptr || generation == 0) return;
    if (md_display_finish_unbind(m_display, static_cast<uint64_t>(generation)) == MD_OK) {
        armWritable();
    }
}

void MirageDisplayItem::closeConnection() {
    if (m_readNotifier != nullptr) {
        delete m_readNotifier;
        m_readNotifier = nullptr;
    }
    if (m_writeNotifier != nullptr) {
        delete m_writeNotifier;
        m_writeNotifier = nullptr;
    }
    if (m_display != nullptr) {
        md_display_free(m_display);
        m_display = nullptr;
    }
    if (m_connected) {
        m_connected = false;
        emit connectedChanged();
    }
    if (m_outputId != 0) {
        m_outputId = 0;
        emit outputIdChanged();
    }
}

void MirageDisplayItem::handleConnectionFailure() {
    closeConnection();
    scheduleReconnect();
}

void MirageDisplayItem::scheduleReconnect() {
    if (isComponentComplete() && m_rendererReady.load() && !m_reconnectTimer.isActive()) {
        m_reconnectTimer.start();
    }
}

void MirageDisplayItem::onConnected(void* userData, uint64_t outputIdValue) {
    auto* self = static_cast<MirageDisplayItem*>(userData);
    self->m_connected = true;
    self->m_outputId = static_cast<qulonglong>(outputIdValue);
    emit self->connectedChanged();
    emit self->outputIdChanged();
}

void MirageDisplayItem::onBuffersReady(void* userData, const md_buffer_pool_t* pool) {
    auto* self = static_cast<MirageDisplayItem*>(userData);
    {
        QMutexLocker locker(&self->m_stateMutex);
        self->m_pendingPool = *pool;
        self->m_hasPendingPool = true;
    }
    self->update();
}

void MirageDisplayItem::onBuffersReleasing(void* userData, const md_buffer_pool_t* pool) {
    auto* self = static_cast<MirageDisplayItem*>(userData);
    bool deferred = self->m_display != nullptr &&
                    md_display_defer_unbind(self->m_display) == MD_OK;
    {
        QMutexLocker locker(&self->m_stateMutex);
        self->m_releaseGeneration = pool->generation;
        self->m_releaseNeedsFinish = deferred;
    }
    self->update();
    if (self->window()) self->window()->update();
}

void MirageDisplayItem::onConfig(void* userData, const md_display_config_t* config) {
    auto* self = static_cast<MirageDisplayItem*>(userData);
    {
        QMutexLocker locker(&self->m_stateMutex);
        self->m_config = *config;
        self->m_hasConfig = true;
    }
    QColor next = QColor::fromRgbF(config->clear_color[0], config->clear_color[1],
                                   config->clear_color[2], config->clear_color[3]);
    if (next != self->m_clearColor) {
        self->m_clearColor = next;
        emit self->clearColorChanged();
    }
    self->update();
}

void MirageDisplayItem::dropFrame(PendingFrame& frame) {
    if (!frame.valid) return;
    if (frame.value.acquire_sync_fd >= 0) close(frame.value.acquire_sync_fd);
    if (frame.value.release_syncobj_fd >= 0) {
        (void)md_display_signal_release_syncobj(frame.value.release_syncobj_fd);
    }
    frame = PendingFrame {};
}

void MirageDisplayItem::onFrame(void* userData, const md_frame_t* frame) {
    auto* self = static_cast<MirageDisplayItem*>(userData);
    {
        QMutexLocker locker(&self->m_stateMutex);
        dropFrame(self->m_pendingFrame);
        self->m_pendingFrame.valid = true;
        self->m_pendingFrame.value = *frame;
    }
    ++self->m_framesReceived;
    emit self->framesReceivedChanged();
    self->update();
}

void MirageDisplayItem::onDisconnected(void* userData, md_result_t reason, const char* message) {
    auto* self = static_cast<MirageDisplayItem*>(userData);
    Q_UNUSED(reason);
    Q_UNUSED(message);
    if (self->m_connected) {
        self->m_connected = false;
        emit self->connectedChanged();
    }
}

bool MirageDisplayItem::importPendingPool(const md_buffer_pool_t& pool) {
    if (m_importer == nullptr || m_imageTargetTexture == nullptr || window() == nullptr) {
        return false;
    }
    if (md_egl_importer_import_pool(m_importer, &pool) != MD_OK) return false;

    const md_egl_imported_pool_t* imported = md_egl_importer_pool(m_importer);
    QOpenGLContext* context = QOpenGLContext::currentContext();
    if (imported == nullptr || context == nullptr) {
        md_egl_importer_release_pool(m_importer);
        return false;
    }

    QOpenGLFunctions* functions = context->functions();
    m_glTextures.resize(static_cast<qsizetype>(imported->buffer_count));
    functions->glGenTextures(static_cast<int>(imported->buffer_count), m_glTextures.data());
    const bool hasAlpha = imported->fourcc == DrmFormatArgb8888;
    const auto options = hasAlpha ? QQuickWindow::TextureHasAlphaChannel
                                  : QQuickWindow::CreateTextureOptions {};

    for (uint32_t index = 0; index < imported->buffer_count; ++index) {
        unsigned int texture = m_glTextures[static_cast<qsizetype>(index)];
        functions->glBindTexture(GL_TEXTURE_2D, texture);
        functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        m_imageTargetTexture(GL_TEXTURE_2D, imported->images[index]);
        QSGTexture* wrapper = QNativeInterface::QSGOpenGLTexture::fromNative(
            texture, window(), QSize(static_cast<int>(imported->width),
                                     static_cast<int>(imported->height)), options);
        if (wrapper == nullptr) {
            releaseRenderPool();
            return false;
        }
        m_qsgTextures.append(wrapper);
    }
    functions->glBindTexture(GL_TEXTURE_2D, 0);
    m_importedGeneration = pool.generation;
    return true;
}

void MirageDisplayItem::releaseRenderPool() {
    if (m_activeReleaseFd >= 0) {
        if (QOpenGLContext::currentContext() != nullptr) {
            QOpenGLContext::currentContext()->functions()->glFinish();
        }
        (void)md_display_signal_release_syncobj(m_activeReleaseFd);
        m_activeReleaseFd = -1;
    }

    {
        QMutexLocker locker(&m_stateMutex);
        dropFrame(m_pendingFrame);
        m_hasPendingPool = false;
    }

    qDeleteAll(m_qsgTextures);
    m_qsgTextures.clear();
    if (!m_glTextures.isEmpty() && QOpenGLContext::currentContext() != nullptr) {
        QOpenGLContext::currentContext()->functions()->glDeleteTextures(
            static_cast<int>(m_glTextures.size()), m_glTextures.constData());
    }
    m_glTextures.clear();
    if (m_importer != nullptr) md_egl_importer_release_pool(m_importer);
    m_importedGeneration = 0;
    m_currentBuffer = -1;
}

void MirageDisplayItem::releaseAfterRendering() {
    if (m_activeReleaseFd < 0) return;
    int releaseFd = m_activeReleaseFd;
    m_activeReleaseFd = -1;
    if (m_importer == nullptr ||
        md_egl_release_after_current_context(m_importer, releaseFd) != MD_OK) {
        return;
    }
}

QSGNode* MirageDisplayItem::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData* data) {
    Q_UNUSED(data);

    uint64_t releaseGeneration = 0;
    bool finishRelease = false;
    {
        QMutexLocker locker(&m_stateMutex);
        releaseGeneration = m_releaseGeneration;
        finishRelease = m_releaseNeedsFinish;
        if (releaseGeneration != 0) {
            m_releaseGeneration = 0;
            m_releaseNeedsFinish = false;
        }
    }
    if (releaseGeneration != 0) {
        delete oldNode;
        oldNode = nullptr;
        releaseRenderPool();
        if (finishRelease) {
            QMetaObject::invokeMethod(this, [this, releaseGeneration]() {
                finishDeferredUnbind(static_cast<qulonglong>(releaseGeneration));
            }, Qt::QueuedConnection);
        }
        return nullptr;
    }

    md_buffer_pool_t pendingPool {};
    bool importPool = false;
    if (m_importer != nullptr) {
        QMutexLocker locker(&m_stateMutex);
        if (m_hasPendingPool) {
            pendingPool = m_pendingPool;
            m_hasPendingPool = false;
            importPool = true;
        }
    }
    if (importPool && !importPendingPool(pendingPool)) {
        delete oldNode;
        oldNode = nullptr;
    }

    PendingFrame frame;
    md_display_config_t config {};
    bool hasConfig = false;
    {
        QMutexLocker locker(&m_stateMutex);
        frame = m_pendingFrame;
        m_pendingFrame = PendingFrame {};
        config = m_config;
        hasConfig = m_hasConfig;
    }

    if (frame.valid) {
        bool valid = frame.value.buffer_generation == m_importedGeneration &&
                     frame.value.buffer_index < static_cast<uint32_t>(m_qsgTextures.size());
        if (!valid || m_importer == nullptr ||
            md_egl_wait_acquire_sync(m_importer, frame.value.acquire_sync_fd) != MD_OK) {
            frame.value.acquire_sync_fd = -1;
            if (frame.value.release_syncobj_fd >= 0) {
                (void)md_display_signal_release_syncobj(frame.value.release_syncobj_fd);
            }
        } else {
            frame.value.acquire_sync_fd = -1;
            if (m_activeReleaseFd >= 0) {
                QOpenGLContext::currentContext()->functions()->glFinish();
                (void)md_display_signal_release_syncobj(m_activeReleaseFd);
            }
            m_activeReleaseFd = frame.value.release_syncobj_fd;
            frame.value.release_syncobj_fd = -1;
            m_currentBuffer = static_cast<int>(frame.value.buffer_index);
        }
    }

    if (m_currentBuffer < 0 || m_currentBuffer >= m_qsgTextures.size()) {
        delete oldNode;
        return nullptr;
    }

    auto* node = static_cast<QSGSimpleTextureNode*>(oldNode);
    if (node == nullptr) {
        node = new QSGSimpleTextureNode();
        node->setFiltering(QSGTexture::Linear);
        node->setOwnsTexture(false);
    }
    node->setTexture(m_qsgTextures[m_currentBuffer]);

    const QRectF bounds = boundingRect();
    const md_egl_imported_pool_t* imported = md_egl_importer_pool(m_importer);
    if (hasConfig && config.source.width > 0.0f && config.source.height > 0.0f) {
        node->setSourceRect(QRectF(config.source.x, config.source.y,
                                   config.source.width, config.source.height));
    } else if (imported != nullptr) {
        node->setSourceRect(QRectF(0.0, 0.0, imported->width, imported->height));
    }

    if (hasConfig && config.destination.width > 0.0f && config.destination.height > 0.0f &&
        m_physicalWidth > 0 && m_physicalHeight > 0) {
        const qreal scaleX = bounds.width() / static_cast<qreal>(m_physicalWidth);
        const qreal scaleY = bounds.height() / static_cast<qreal>(m_physicalHeight);
        node->setRect(QRectF(config.destination.x * scaleX,
                             config.destination.y * scaleY,
                             config.destination.width * scaleX,
                             config.destination.height * scaleY));
    } else {
        node->setRect(bounds);
    }
    return node;
}

uint32_t MirageDisplayItem::linuxButton(Qt::MouseButton button) {
    switch (button) {
    case Qt::LeftButton: return BtnLeft;
    case Qt::RightButton: return BtnRight;
    case Qt::MiddleButton: return BtnMiddle;
    case Qt::BackButton: return BtnSide;
    case Qt::ForwardButton: return BtnExtra;
    default: return 0;
    }
}

bool MirageDisplayItem::mapPointer(const QPointF& scenePosition, float& x, float& y) const {
    const QRectF bounds = boundingRect();
    if (bounds.width() <= 0.0 || bounds.height() <= 0.0) return false;
    const QPointF local = mapFromScene(scenePosition);
    if (!bounds.contains(local)) return false;
    x = static_cast<float>(local.x() * static_cast<qreal>(m_physicalWidth) / bounds.width());
    y = static_cast<float>(local.y() * static_cast<qreal>(m_physicalHeight) / bounds.height());
    return true;
}

bool MirageDisplayItem::eventFilter(QObject* watched, QEvent* event) {
    if (!m_pointerForwarding || watched != window() || m_display == nullptr ||
        md_display_connection_state(m_display) != MD_CONNECTION_READY) {
        return false;
    }

    auto ensureEnter = [this](float x, float y, uint64_t timestamp) {
        if (m_pointerInside) return;
        m_pointerInside = true;
        (void)md_display_send_pointer_enter(m_display, x, y, timestamp);
    };

    switch (event->type()) {
    case QEvent::MouseMove: {
        auto* mouse = static_cast<QMouseEvent*>(event);
        float x = 0.0f;
        float y = 0.0f;
        const uint64_t timestamp = static_cast<uint64_t>(mouse->timestamp()) * 1000u;
        if (!mapPointer(mouse->scenePosition(), x, y)) {
            if (m_pointerInside) {
                m_pointerInside = false;
                (void)md_display_send_pointer_leave(m_display, timestamp);
            }
            break;
        }
        ensureEnter(x, y, timestamp);
        (void)md_display_send_pointer_motion(m_display, x, y, timestamp, 0);
        break;
    }
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease: {
        auto* mouse = static_cast<QMouseEvent*>(event);
        float x = 0.0f;
        float y = 0.0f;
        if (!mapPointer(mouse->scenePosition(), x, y)) break;
        uint32_t button = linuxButton(mouse->button());
        if (button == 0) break;
        const uint64_t timestamp = static_cast<uint64_t>(mouse->timestamp()) * 1000u;
        ensureEnter(x, y, timestamp);
        md_button_state_t state = event->type() == QEvent::MouseButtonPress
                                      ? MD_BUTTON_PRESSED
                                      : MD_BUTTON_RELEASED;
        (void)md_display_send_pointer_button(m_display, x, y, button, state, timestamp, 0);
        break;
    }
    case QEvent::Wheel: {
        auto* wheel = static_cast<QWheelEvent*>(event);
        float x = 0.0f;
        float y = 0.0f;
        if (!mapPointer(wheel->position(), x, y)) break;
        const QPoint angle = wheel->angleDelta();
        const uint64_t timestamp = static_cast<uint64_t>(wheel->timestamp()) * 1000u;
        ensureEnter(x, y, timestamp);
        (void)md_display_send_pointer_axis(m_display, x, y,
                                           static_cast<float>(angle.x()) / 120.0f,
                                           static_cast<float>(angle.y()) / 120.0f,
                                           MD_AXIS_WHEEL, timestamp, 0);
        break;
    }
    case QEvent::Leave:
        if (m_pointerInside) {
            m_pointerInside = false;
            (void)md_display_send_pointer_leave(m_display, 0);
        }
        break;
    default:
        return false;
    }
    armWritable();
    return false;
}
