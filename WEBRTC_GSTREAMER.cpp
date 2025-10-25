#include <gst/gst.h>
#include <gst/webrtc/webrtc.h>
#include <gst/sdp/sdp.h>
#include <glib.h>
#include <curl/curl.h>
#include <iostream>
#include <string>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

static GMainLoop *loop = nullptr;
static GstElement *pipeline = nullptr;
static GstElement *webrtcbin = nullptr;

// =========================================================
// HTTPレスポンス用バッファ
// ---------------------------------------------------------
// 【目的】
// curl通信でサーバーから受け取ったHTTPレスポンスデータを
// 文字列バッファに蓄積するための関数。
// 【処理内容】
// curlがデータを受信するたびにこの関数が呼ばれ、
// 受け取ったデータをstd::stringに追記していく。
// =========================================================
static size_t WriteCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    ((std::string *)userp)->append((char *)contents, size * nmemb);
    return size * nmemb;
}

// =========================================================
// Offer SDP を OpenAI Realtime API に送信
// ---------------------------------------------------------
// 【目的】
// ローカルで生成したWebRTC SDP(Offer)をOpenAIのRealtime APIに送信し、
// サーバー側の応答(Answer SDP)を取得する。
// 【処理内容】
// - AuthorizationヘッダーにEphemeral Keyを設定
// - SDPデータをPOSTで送信
// - OpenAIから返ってきたSDP Answer文字列を返す
// =========================================================
static std::string post_offer_and_get_answer(const std::string &offer_sdp, const std::string &ephemeral_key) {
    CURL *curl = curl_easy_init();
    if (!curl) return "";

    std::string readBuffer;
    struct curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, ("Authorization: Bearer " + ephemeral_key).c_str()); // 認証ヘッダの設定
    headers = curl_slist_append(headers, "Content-Type: application/sdp"); // 送信データ形式（SDPフォーマット）指定
    headers = curl_slist_append(headers, "Accept: application/sdp"); // 受信データ形式（SDPフォーマット）指定

    // WebRTCのOffer SDPをOpenAI Realtime APIに送るためのHTTP POST設定
    curl_easy_setopt(curl, CURLOPT_URL, "https://api.openai.com/v1/realtime?model=gpt-4o-realtime-preview"); // HTTPリクエスト送信先URL（エンドポイント）指定
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers); // HTTPヘッダ（認証・データ形式など）設定
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, offer_sdp.c_str()); // POSTデータ（送信内容）設定
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, offer_sdp.size()); // データサイズ設定
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback); // 受信データ処理関数設定
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer); // 受信先バッファ設定

    // OpenAIサーバ通信（HTTP POST）実施
    CURLcode res = curl_easy_perform(curl);
    if (res == CURLE_OK) {
        std::cout << "[OK] HTTP request success.\n";
        std::cout << "Response: " << readBuffer << std::endl;
    }
    else {
        std::cerr << "[ERR] CURL failed: " << curl_easy_strerror(res) << std::endl;
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    return readBuffer;
}

// =========================================================
// Ephemeral Key を取得
// ---------------------------------------------------------
// 【目的】
// OpenAIのAPIキー(通常キー)を使って、
// 一時的に使用できるEphemeral Keyをサーバーから取得する。
// 【処理内容】
// - /v1/realtime/sessions に対してPOST
// - JSONレスポンスから client_secret.value を取り出す
// - Ephemeral Keyを返却
// =========================================================
static std::string create_ephemeral_key() {
    std::string ephemeral_key;

    CURL *curl = curl_easy_init();
    if (!curl) return "";

    std::string readBuffer;
    std::string api_key = std::getenv("OPENAI_API_KEY");
    std::string url = "https://api.openai.com/v1/realtime/sessions";
    std::string payload = R"({"model": "gpt-4o-realtime-preview"})";

    struct curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, ("Authorization: Bearer " + api_key).c_str());
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str()); // HTTPリクエスト先URL設定
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers); //HTTPリクエストヘッダ設定
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str()); // 送信データ（POSTボディ）設定
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, // 受信データ処理関数（受信データを文字列に追記）設定
                     +[](void *contents, size_t size, size_t nmemb, void *userp) -> size_t {
                         ((std::string *)userp)->append((char *)contents, size * nmemb);
                         return size * nmemb;
                     });
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer); // 受信データの格納先指定（受信したレスポンスデータをreadBufferに蓄積）

    CURLcode res = curl_easy_perform(curl); // 設定済みのHTTPリクエストを実行
    if (res == CURLE_OK) {
        try {
            auto json_resp = nlohmann::json::parse(readBuffer);
            if (json_resp.contains("client_secret") && json_resp["client_secret"].contains("value")) {
                ephemeral_key = json_resp["client_secret"]["value"]; // 一時的な認証キー抽出
                std::cout << "[KEY] Ephemeral key: " << ephemeral_key.substr(0, 20) << "...\n";
            } else {
                std::cerr << "[ERR] Unexpected response: " << readBuffer << std::endl;
            }
        } catch (const std::exception &e) {
            std::cerr << "[ERR] JSON parse error: " << e.what() << std::endl;
        }
    } else {
        std::cerr << "[ERR] CURL request failed: " << curl_easy_strerror(res) << std::endl;
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return ephemeral_key;
}

// =========================================================
// DataChannel open時に呼び出されるコールバック関数
// =========================================================
static void on_data_channel_open(GstElement *channel, gpointer) {
    std::cout << "[DC] DataChannel opened. Sending session.update...\n";

    json event = {
        {"type", "session.update"},
        {"session", {
            {"input_audio_format", "pcm24"}, // 入力音声フォーマットをPCM 24bitに設定
            {"input_text", true}, // テキスト入力も許可（音声と両方扱う）
            {"output_audio_format", "pcm24"}, // 出力音声をPCM 24bitで受け取る
            {"output_text", true}, // テキスト出力も受け取る
            {"voice_activity_detection", { {"mode", "advanced"} }}, // VAD（音声区間検出）設定（mode: advanced は高精度）
            {"instructions", "You are connected from C++ GStreamer client with advanced VAD enabled (PCM24)."} // モデルへの指示文（プロンプト的役割）
        }}
    };

    std::string message = event.dump(); // jsonオブジェクトを文字列に変換
    g_signal_emit_by_name(channel, "send-string", message.c_str()); // WebRTC DataChannel を通じてイベントを送

    std::cout << "[SEND] session.update sent: " << message << std::endl;
}

// =========================================================
// WebRTC ネゴシエーション処理
// ---------------------------------------------------------
// 【目的】
// webrtcbinが通信を開始する際（on-negotiation-neededシグナル発火時）に、
// Offer SDPを生成し、OpenAIへ送信して通信相手(Answer)を確立する。
// 【処理内容】
// - DataChannelを作成
// - Offer SDPを生成
// - OpenAIからAnswer SDPを受信し、リモート記述を設定
// =========================================================
static void on_negotiation_needed(GstElement *webrtc, gpointer) {
    std::cout << "[NEG] on-negotiation-needed\n";

    // Offer作成より先にDataChannelを作成
    // WebRTCのデータ通信路（双方向メッセージ送信に使う）
    GstElement *data_channel = nullptr;
    g_signal_emit_by_name(webrtc, "create-data-channel", "data", nullptr, &data_channel); // シグナル名："create-data-channel"、DataChannel名："data"
 
    if (data_channel) {
        std::cout << "[DC] DataChannel created\n";

        g_signal_connect(data_channel, "on-open", G_CALLBACK(on_data_channel_open), nullptr); // DataChannel開通(open)時のコールバック関数登録

        gst_object_unref(data_channel); // webrtcbinが参照保持（data_channelの参照は放棄）
    }
    else {
        // =========================================================
        // DataChannel生成失敗時の処理
        // ---------------------------------------------------------
        // 【目的】
        // DataChannelが作成できなかった場合にユーザーへ明示的に通知。
        // WebRTCの通信確立後も双方向メッセージが利用できない状態となるため、
        // ここでログ出力して後続処理を継続する。
        // =========================================================
        std::cerr << "[ERR] Failed to create DataChannel. "
                  << "session.update cannot be sent.\n";
    }

    // Offer生成用Promise（非同期でOffer作成完了後にコールバック実行）　※Offer SDP作成→サーバ送信→Answer設定
    GstPromise *promise = gst_promise_new_with_change_func(
        [](GstPromise *promise, gpointer) {
            // Offer SDP(接続条件)を取得しoffer変数に格納
            const GstStructure *reply = gst_promise_get_reply(promise);
            GstWebRTCSessionDescription *offer = nullptr;
            gst_structure_get(reply, "offer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &offer, NULL);
            gst_promise_unref(promise);

            // Offer SDP文字列を取得して出力
            gchar *sdp_str = gst_sdp_message_as_text(offer->sdp);
            std::cout << "[SDP] Local offer set\n";
            std::cout << "---- SDP OFFER ----\n" << sdp_str << "\n---- END ----\n";

            // ローカル側にOffer SDPを設定
            g_signal_emit_by_name(webrtcbin, "set-local-description", offer, NULL);

            // Ephemeral Keyを取得し、OfferをOpenAIに送信してAnswer受信
            std::string ephemeral_key = create_ephemeral_key();
            std::string answer_sdp    = post_offer_and_get_answer(sdp_str, ephemeral_key);

            std::cout << "---- SDP ANSWER ----\n" << answer_sdp << "\n---- END ----\n";

            // Answer SDPをwebrtcbinに設定（通信確立）
            if (!answer_sdp.empty()) {
                GstSDPMessage *sdp = NULL;
                gst_sdp_message_new(&sdp); // SDPメッセージオブジェクト作成
                gst_sdp_message_parse_buffer( // 受信SDP文字列をパース（解析）
                    (const guint8 *)answer_sdp.c_str(), answer_sdp.size(), sdp);

                GstWebRTCSessionDescription *answer = // GstWebRTCSessionDescription変換
                    gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_ANSWER, sdp);
                g_signal_emit_by_name(webrtcbin, "set-remote-description", answer, NULL); // webrtcbin に適用（リモートSDPを設定）
                std::cout << "[SDP] Remote answer set.\n";
                gst_webrtc_session_description_free(answer); // answer オブジェクト解放
            }

            g_free(sdp_str); // SDP文字列（文字列バッファ）を解放
            gst_webrtc_session_description_free(offer); // offer（WebRTCセッション記述子）解放
        },
        NULL, NULL);

    // Offer生成を要求（要求後、Promiseコールバック（gst_promise_new_with_change_func）でOfferを取得してHTTP送信）
    g_signal_emit_by_name(webrtc, "create-offer", NULL, promise);
}

// =========================================================
// ICE Connection State Change
// ---------------------------------------------------------
// 【目的】
// ICE接続状態（New, Checking, Connectedなど）が変化したときに
// 状態をログ出力する。
// =========================================================
static void on_ice_state_change(GstElement *webrtcbin, GParamSpec *pspec, gpointer user_data) {
    GstWebRTCICEConnectionState state;
    g_object_get(webrtcbin, "ice-connection-state", &state, NULL);

    switch (state) {
        // ICE処理が始まった直後
        case GST_WEBRTC_ICE_CONNECTION_STATE_NEW:
            std::cout << "[ICE] New\n";
            break;
        // 通信経路をテスト中
        case GST_WEBRTC_ICE_CONNECTION_STATE_CHECKING:
            std::cout << "[ICE] Checking\n";
            break;
        // 通信確立
        case GST_WEBRTC_ICE_CONNECTION_STATE_CONNECTED:
            std::cout << "[ICE] Connected\n";
            break;
        // 最適な経路確立（安定状態）
        case GST_WEBRTC_ICE_CONNECTION_STATE_COMPLETED:
            std::cout << "[ICE] Completed\n";
            break;
        // 通信失敗
        case GST_WEBRTC_ICE_CONNECTION_STATE_FAILED:
            std::cout << "[ICE] Failed\n";
            break;
        // 一時的に接続断
        case GST_WEBRTC_ICE_CONNECTION_STATE_DISCONNECTED:
            std::cout << "[ICE] Disconnected\n";
            break;
        // ICE処理終了（セッション終了）
        case GST_WEBRTC_ICE_CONNECTION_STATE_CLOSED:
            std::cout << "[ICE] Closed\n";
            break;
        default:
            std::cout << "[ICE] Unknown or new state: " << state << std::endl;
            break;
    }
}

// ---------------------------------------------------------
// ICE candidate 生成時の処理
// 【目的】
// WebRTCがローカルネットワークまたはSTUNを通じて
// 利用可能な通信候補(ICE candidate)を検出したときに呼ばれ、
// 候補情報をログ出力する。
// ---------------------------------------------------------
static void on_ice_candidate(GstElement *webrtc, guint mlineindex, gchar *candidate, gpointer user_data) {
    std::cout << "[ICE] Candidate gathered: " << candidate << std::endl;
}

// =========================================================
// main()
// ---------------------------------------------------------
// 【目的】
// GStreamerとWebRTCの初期化を行い、OpenAI Realtime APIとの
// PeerConnectionを確立するメイン処理。
// 【処理内容】
// - GStreamer初期化
// - webrtcbin生成とSTUN設定
// - コールバック関数の登録
// - イベントループ実行
// =========================================================
int main(int argc, char *argv[]) {
    gst_init(&argc, &argv);
    loop = g_main_loop_new(NULL, FALSE); // メインループを用意（開始は後で）

    // GStreamerパイプラインとwebrtcbinの作成
    pipeline = gst_pipeline_new("pipeline");
    webrtcbin = gst_element_factory_make("webrtcbin", "webrtcbin");  // WebRTC通信ノードを生成

    // STUN サーバ設定（Googleの無料STUNサーバを利用）  ※自端末のグローバルIP/portを取得
    g_object_set(webrtcbin,
        "stun-server",  "stun://stun.l.google.com:19302",
        "bundle-policy", GST_WEBRTC_BUNDLE_POLICY_MAX_BUNDLE,
        NULL);

    gst_bin_add(GST_BIN(pipeline), webrtcbin); // webrtcbin をパイプラインに登録

    // --- WebRTC イベント接続 ---
    // ネゴシエーション開始時（DataChannelを作成→SDP Offerを生成→Offerをサーバ送信（POST）→SDP Answer受信→Answerをwebrtcbinに設定）
    g_signal_connect(webrtcbin, "on-negotiation-needed", G_CALLBACK(on_negotiation_needed), NULL);

    // ICEコネクション状態変化通知
    g_signal_connect(webrtcbin, "notify::ice-connection-state", G_CALLBACK(on_ice_state_change), NULL);

    // ICE候補生成通知（通信経路候補（ICE candidate）が見つかったら通知されるイベント登録）
    g_signal_connect(webrtcbin, "on-ice-candidate", G_CALLBACK(on_ice_candidate), NULL);

    // パイプラインを再生状態に設定し、メインループを開始
    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    std::cout << "Running main loop... (Ctrl+C to quit)\n";
    g_main_loop_run(loop);

    // 終了時のリソース解放
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    g_main_loop_unref(loop);
    return 0;
}
