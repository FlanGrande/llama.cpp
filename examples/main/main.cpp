// Defines sigaction on msys:
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "common.h"
#include "llama.h"
#include "build-info.h"

#include <cassert>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>
#include <regex>

#if defined (__unix__) || (defined (__APPLE__) && defined (__MACH__))
#include <signal.h>
#include <unistd.h>
#elif defined (_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <signal.h>
#endif

#if defined(_MSC_VER)
#pragma warning(disable: 4244 4267) // possible loss of data
#endif

static console_state con_st;
static llama_context ** g_ctx;

static bool is_interacting = false;
PROCESS_INFORMATION whisper_process_info;
HANDLE whisper_process_stdoutRead, whisper_process_stdoutWrite;

#if defined (__unix__) || (defined (__APPLE__) && defined (__MACH__)) || defined (_WIN32)
void sigint_handler(int signo) {
    if (signo == SIGINT) {
        if (!is_interacting) {
            is_interacting=true;
        } else {
            console_cleanup(con_st);
            printf("\n");
            llama_print_timings(*g_ctx);
            _exit(130);
        }
    }
}
#endif

int generate_random_speaker_id() {
    int speaker_idxs[] = {273, 267, 226, 241, 260, 298, 335, 293};
    int arraySize = sizeof(speaker_idxs) / sizeof(speaker_idxs[0]);

    // Seed the random number generator
    std::srand(static_cast<unsigned int>(std::time(nullptr)));

    // Generate a random index within the array bounds
    int randomIndex = std::rand() % arraySize;

    // Select a random number from the array
    return speaker_idxs[randomIndex];
}

void start_tts(std::string text_to_read, bool debug_tts = false) {
    // Run tts --text "AI_answer_to_TTS" on powershell to hear the voice
    int speaker_id = generate_random_speaker_id();
    std::string generate_TTS_command = "cmd /C \"tts --text \"" + text_to_read + "\" --model_name \"tts_models/en/vctk/vits\" --use_cuda true --speaker_idx p" + std::to_string(speaker_id);
    std::string play_TTS_command = "powershell -c (New-Object Media.SoundPlayer \".\\tts_output.wav\").PlaySync() ^| out-null\"";
    std::string start_TTS_command = generate_TTS_command + " && " + play_TTS_command;

    STARTUPINFO si = { sizeof(si) };
    PROCESS_INFORMATION pi; 
    DWORD creationFlags = CREATE_NEW_CONSOLE;
    si.cb = sizeof(STARTUPINFO); 
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    if(debug_tts) si.wShowWindow = SW_SHOW;

    if(!CreateProcess(NULL,
    const_cast<char*>(start_TTS_command.c_str()),
    NULL,
    NULL,
    TRUE,
    creationFlags,
    NULL,
    NULL,
    &si,
    &pi))
    {
        std::cout << "Error printing output process" << GetLastError() << std::endl;
    }
    
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
}

PROCESS_INFORMATION start_whisper() {
    std::string start_whisper_command = "cmd /C E:\\Proyectos\\AI\\llama.cpp\\WhisperModule\\MicrophoneCS.exe -m E:\\Proyectos\\AI\\models\\ggml-tiny.en.bin --device 1 -nt -nc";

    STARTUPINFO si = { sizeof(si) };
    PROCESS_INFORMATION pi; 
    SECURITY_ATTRIBUTES saAttr;

    // Create a pipe for the child process's stdout
    saAttr.nLength = sizeof(saAttr);
    saAttr.bInheritHandle = TRUE;
    saAttr.lpSecurityDescriptor = NULL;

    if(!CreatePipe(&whisper_process_stdoutRead, &whisper_process_stdoutWrite, &saAttr, 0))
    {
        std::cout << "Error creating pipe: " << GetLastError() << std::endl;
    }
    
    si.cb = sizeof(STARTUPINFO); 
    si.wShowWindow = SW_HIDE;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = whisper_process_stdoutWrite;
    si.hStdError = whisper_process_stdoutWrite;
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.dwFlags |= STARTF_USESTDHANDLES;

    if(!CreateProcess(NULL,
    const_cast<char*>(start_whisper_command.c_str()),
    NULL,
    NULL,
    TRUE,
    0,
    NULL,
    NULL,
    &si,
    &pi))
    {
        std::cout << "Error printing output process" << GetLastError() << std::endl;
    }
    
    // Is this OK?
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(whisper_process_stdoutWrite);
    return pi;
}

void stop_process(PROCESS_INFORMATION& process_info) {
    if(process_info.hProcess == NULL)
    {
        return;
    }

    CloseHandle(whisper_process_stdoutRead);
    
    // Terminate process
    TerminateProcess(process_info.hProcess, 0);
    CloseHandle(process_info.hProcess);
    CloseHandle(process_info.hThread);
}

bool has_over_and_out_been_whispered(std::string line)
{
    std::string line_lower = line;
    std::transform(line_lower.begin(), line_lower.end(), line_lower.begin(), ::tolower);

    if (line_lower.find("over and out") != std::string::npos) {
        return true;
    }
    else {
        return false;
    }
}

void sanitize_buffer(std::string &buffer)
{
    std::string buffer_str = buffer;
    std::string buffer_str_lower = buffer_str;
    std::transform(buffer_str_lower.begin(), buffer_str_lower.end(), buffer_str_lower.begin(), ::tolower);

    if (buffer_str_lower.find("over and out") != std::string::npos) {
        buffer_str = buffer_str.substr(0, buffer_str_lower.find("over and out"));
    }

    // Remove text between brackets, text between parenthesis, special characters and \r and \n.
    buffer_str = std::regex_replace(buffer_str, std::regex("\\[.*?\\]"), "");
    buffer_str = std::regex_replace(buffer_str, std::regex("\\(.*?\\)"), "");
    buffer_str = std::regex_replace(buffer_str, std::regex("\\s+"), " ");
    buffer_str = std::regex_replace(buffer_str, std::regex("\\r\\n|\\r|\\n"), "");

    // Remove leading and trailing spaces
    buffer_str = std::regex_replace(buffer_str, std::regex("^ +| +$|( ) +"), "$1");

    // Append \n
    buffer_str += "\n";
    buffer_str += "\n";

    buffer = buffer_str;
}

int main(int argc, char ** argv) {
    gpt_params params;

    if (gpt_params_parse(argc, argv, params) == false) {
        return 1;
    }

    // save choice to use color for later
    // (note for later: this is a slightly awkward choice)
    con_st.use_color = params.use_color;
    con_st.multiline_input = params.multiline_input;
    console_init(con_st);
    atexit([]() { console_cleanup(con_st); });

    if (params.perplexity) {
        printf("\n************\n");
        printf("%s: please use the 'perplexity' tool for perplexity calculations\n", __func__);
        printf("************\n\n");

        return 0;
    }

    if (params.embedding) {
        printf("\n************\n");
        printf("%s: please use the 'embedding' tool for embedding calculations\n", __func__);
        printf("************\n\n");

        return 0;
    }

    if (params.n_ctx > 2048) {
        fprintf(stderr, "%s: warning: model does not support context sizes greater than 2048 tokens (%d specified);"
                "expect poor results\n", __func__, params.n_ctx);
    } else if (params.n_ctx < 8) {
        fprintf(stderr, "%s: warning: minimum context size is 8, using minimum size.\n", __func__);
        params.n_ctx = 8;
    }

    fprintf(stderr, "%s: build = %d (%s)\n", __func__, BUILD_NUMBER, BUILD_COMMIT);

    if (params.seed < 0) {
        params.seed = time(NULL);
    }

    fprintf(stderr, "%s: seed  = %d\n", __func__, params.seed);

    std::mt19937 rng(params.seed);
    if (params.random_prompt) {
        params.prompt = gpt_random_prompt(rng);
    }

    llama_init_backend(params.numa);

    llama_model * model;
    llama_context * ctx;
    g_ctx = &ctx;

    // load the model and apply lora adapter, if any
    std::tie(model, ctx) = llama_init_from_gpt_params(params);
    if (model == NULL) {
        fprintf(stderr, "%s: error: unable to load model\n", __func__);
        return 1;
    }

    // print system information
    {
        fprintf(stderr, "\n");
        fprintf(stderr, "system_info: n_threads = %d / %d | %s\n",
                params.n_threads, std::thread::hardware_concurrency(), llama_print_system_info());
    }

    // determine the maximum memory usage needed to do inference for the given n_batch and n_predict parameters
    // uncomment the "used_mem" line in llama.cpp to see the results
    if (params.mem_test) {
        {
            const std::vector<llama_token> tmp(params.n_batch, llama_token_bos());
            llama_eval(ctx, tmp.data(), tmp.size(), 0, params.n_threads);
        }

        {
            const std::vector<llama_token> tmp = { 0, };
            llama_eval(ctx, tmp.data(), tmp.size(), params.n_predict - 1, params.n_threads);
        }

        llama_print_timings(ctx);
        llama_free(ctx);
        llama_free_model(model);

        return 0;
    }

    // export the cgraph and exit
    if (params.export_cgraph) {
        llama_eval_export(ctx, "llama.ggml");
        llama_free(ctx);
        llama_free_model(model);

        return 0;
    }

    std::string path_session = params.path_prompt_cache;
    std::vector<llama_token> session_tokens;

    if (!path_session.empty()) {
        fprintf(stderr, "%s: attempting to load saved session from '%s'\n", __func__, path_session.c_str());

        // fopen to check for existing session
        FILE * fp = std::fopen(path_session.c_str(), "rb");
        if (fp != NULL) {
            std::fclose(fp);

            session_tokens.resize(params.n_ctx);
            size_t n_token_count_out = 0;
            if (!llama_load_session_file(ctx, path_session.c_str(), session_tokens.data(), session_tokens.capacity(), &n_token_count_out)) {
                fprintf(stderr, "%s: error: failed to load session file '%s'\n", __func__, path_session.c_str());
                return 1;
            }
            session_tokens.resize(n_token_count_out);
            llama_set_rng_seed(ctx, params.seed);

            fprintf(stderr, "%s: loaded a session with prompt size of %d tokens\n", __func__, (int) session_tokens.size());
        } else {
            fprintf(stderr, "%s: session file does not exist, will create\n", __func__);
        }
    }

    // tokenize the prompt
    std::vector<llama_token> embd_inp;

    if (params.interactive_first || params.instruct || !params.prompt.empty() || session_tokens.empty()) {
        // Add a space in front of the first character to match OG llama tokenizer behavior
        params.prompt.insert(0, 1, ' ');

        embd_inp = ::llama_tokenize(ctx, params.prompt, true);
    } else {
        embd_inp = session_tokens;
    }

    const int n_ctx = llama_n_ctx(ctx);

    if ((int) embd_inp.size() > n_ctx - 4) {
        fprintf(stderr, "%s: error: prompt is too long (%d tokens, max %d)\n", __func__, (int) embd_inp.size(), n_ctx - 4);
        return 1;
    }

    // debug message about similarity of saved session, if applicable
    size_t n_matching_session_tokens = 0;
    if (session_tokens.size()) {
        for (llama_token id : session_tokens) {
            if (n_matching_session_tokens >= embd_inp.size() || id != embd_inp[n_matching_session_tokens]) {
                break;
            }
            n_matching_session_tokens++;
        }
        if (params.prompt.empty() && n_matching_session_tokens == embd_inp.size()) {
            fprintf(stderr, "%s: using full prompt from session file\n", __func__);
        } else if (n_matching_session_tokens >= embd_inp.size()) {
            fprintf(stderr, "%s: session file has exact match for prompt!\n", __func__);
        } else if (n_matching_session_tokens < (embd_inp.size() / 2)) {
            fprintf(stderr, "%s: warning: session file has low similarity to prompt (%zu / %zu tokens); will mostly be reevaluated\n",
                __func__, n_matching_session_tokens, embd_inp.size());
        } else {
            fprintf(stderr, "%s: session file matches %zu / %zu tokens of prompt\n",
                __func__, n_matching_session_tokens, embd_inp.size());
        }
    }

    // if we will use the cache for the full prompt without reaching the end of the cache, force
    // reevaluation of the last token token to recalculate the cached logits
    if (!embd_inp.empty() && n_matching_session_tokens == embd_inp.size() &&
            session_tokens.size() > embd_inp.size()) {
        session_tokens.resize(embd_inp.size() - 1);
    }

    // number of tokens to keep when resetting context
    if (params.n_keep < 0 || params.n_keep > (int) embd_inp.size() || params.instruct) {
        params.n_keep = (int)embd_inp.size();
    }

    // prefix & suffix for instruct mode
    const auto inp_pfx = ::llama_tokenize(ctx, "\n\n### Instruction:\n\n", true);
    const auto inp_sfx = ::llama_tokenize(ctx, "\n\n### Response:\n\n", false);

    // in instruct mode, we inject a prefix and a suffix to each input by the user
    if (params.instruct) {
        params.interactive_first = true;
        params.antiprompt.push_back("### Instruction:\n\n");
    }

    // enable interactive mode if interactive start is specified
    if (params.interactive_first) {
        params.interactive = true;
    }

    // determine newline token
    auto llama_token_newline = ::llama_tokenize(ctx, "\n", false);

    if (params.verbose_prompt) {
        fprintf(stderr, "\n");
        fprintf(stderr, "%s: prompt: '%s'\n", __func__, params.prompt.c_str());
        fprintf(stderr, "%s: number of tokens in prompt = %zu\n", __func__, embd_inp.size());
        for (int i = 0; i < (int) embd_inp.size(); i++) {
            fprintf(stderr, "%6d -> '%s'\n", embd_inp[i], llama_token_to_str(ctx, embd_inp[i]));
        }
        if (params.n_keep > 0) {
        fprintf(stderr, "%s: static prompt based on n_keep: '", __func__);
            for (int i = 0; i < params.n_keep; i++) {
                fprintf(stderr, "%s", llama_token_to_str(ctx, embd_inp[i]));
            }
            fprintf(stderr, "'\n");
        }
        fprintf(stderr, "\n");
    }

    if (params.interactive) {
#if defined (__unix__) || (defined (__APPLE__) && defined (__MACH__))
        struct sigaction sigint_action;
        sigint_action.sa_handler = sigint_handler;
        sigemptyset (&sigint_action.sa_mask);
        sigint_action.sa_flags = 0;
        sigaction(SIGINT, &sigint_action, NULL);
#elif defined (_WIN32)
        auto console_ctrl_handler = +[](DWORD ctrl_type) -> BOOL {
            return (ctrl_type == CTRL_C_EVENT) ? (sigint_handler(SIGINT), true) : false;
        };
        SetConsoleCtrlHandler(static_cast<PHANDLER_ROUTINE>(console_ctrl_handler), true);
#endif

        fprintf(stderr, "%s: interactive mode on.\n", __func__);

        if (params.antiprompt.size()) {
            for (auto antiprompt : params.antiprompt) {
                fprintf(stderr, "Reverse prompt: '%s'\n", antiprompt.c_str());
            }
        }

        if (!params.input_prefix.empty()) {
            fprintf(stderr, "Input prefix: '%s'\n", params.input_prefix.c_str());
        }

        if (!params.input_suffix.empty()) {
            fprintf(stderr, "Input suffix: '%s'\n", params.input_suffix.c_str());
        }
    }
    fprintf(stderr, "sampling: repeat_last_n = %d, repeat_penalty = %f, presence_penalty = %f, frequency_penalty = %f, top_k = %d, tfs_z = %f, top_p = %f, typical_p = %f, temp = %f, mirostat = %d, mirostat_lr = %f, mirostat_ent = %f\n",
            params.repeat_last_n, params.repeat_penalty, params.presence_penalty, params.frequency_penalty, params.top_k, params.tfs_z, params.top_p, params.typical_p, params.temp, params.mirostat, params.mirostat_eta, params.mirostat_tau);
    fprintf(stderr, "generate: n_ctx = %d, n_batch = %d, n_predict = %d, n_keep = %d\n", n_ctx, params.n_batch, params.n_predict, params.n_keep);
    fprintf(stderr, "\n\n");

    // TODO: replace with ring-buffer
    std::vector<llama_token> last_n_tokens(n_ctx);
    std::fill(last_n_tokens.begin(), last_n_tokens.end(), 0);

    if (params.interactive) {
        const char *control_message;
        if (con_st.multiline_input) {
            control_message = " - To return control to LLaMa, end your input with '\\'.\n"
                              " - To return control without starting a new line, end your input with '/'.\n";
        } else {
            control_message = " - Press Return to return control to LLaMa.\n"
                              " - To return control without starting a new line, end your input with '/'.\n"
                              " - If you want to submit another line, end your input with '\\'.\n";
        }
        fprintf(stderr, "== Running in interactive mode. ==\n"
#if defined (__unix__) || (defined (__APPLE__) && defined (__MACH__)) || defined (_WIN32)
               " - Press Ctrl+C to interject at any time.\n"
#endif
               "%s\n", control_message);

        is_interacting = params.interactive_first;
    }

    bool is_antiprompt        = false;
    bool input_echo           = true;
    bool need_to_save_session = !path_session.empty() && n_matching_session_tokens < embd_inp.size();

    int n_past             = 0;
    int n_remain           = params.n_predict;
    int n_consumed         = 0;
    int n_session_consumed = 0;

    bool has_original_prompt_been_read = false;
    std::string AI_answer_to_TTS = "";

    // the first thing we will do is to output the prompt, so set color accordingly
    console_set_color(con_st, CONSOLE_COLOR_PROMPT);

    std::vector<llama_token> embd;

    // do one empty run to warm up the model
    {
        const std::vector<llama_token> tmp = { llama_token_bos(), };
        llama_eval(ctx, tmp.data(), tmp.size(), 0, params.n_threads);
        llama_reset_timings(ctx);
    }

    while ((n_remain != 0 && !is_antiprompt) || params.interactive) {
        // predict
        if (embd.size() > 0) {
            // Note: n_ctx - 4 here is to match the logic for commandline prompt handling via
            // --prompt or --file which uses the same value.
            auto max_embd_size = n_ctx - 4;
            // Ensure the input doesn't exceed the context size by truncating embd if necessary.
            if ((int)embd.size() > max_embd_size) {
                auto skipped_tokens = embd.size() - max_embd_size;
                console_set_color(con_st, CONSOLE_COLOR_ERROR);
                printf("<<input too long: skipped %zu token%s>>", skipped_tokens, skipped_tokens != 1 ? "s" : "");
                console_set_color(con_st, CONSOLE_COLOR_DEFAULT);
                fflush(stdout);
                embd.resize(max_embd_size);
            }

            // infinite text generation via context swapping
            // if we run out of context:
            // - take the n_keep first tokens from the original prompt (via n_past)
            // - take half of the last (n_ctx - n_keep) tokens and recompute the logits in batches
            if (n_past + (int) embd.size() > n_ctx) {
                const int n_left = n_past - params.n_keep;

                // always keep the first token - BOS
                n_past = std::max(1, params.n_keep);

                // insert n_left/2 tokens at the start of embd from last_n_tokens
                embd.insert(embd.begin(), last_n_tokens.begin() + n_ctx - n_left/2 - embd.size(), last_n_tokens.end() - embd.size());

                // stop saving session if we run out of context
                path_session.clear();

                //printf("\n---\n");
                //printf("resetting: '");
                //for (int i = 0; i < (int) embd.size(); i++) {
                //    printf("%s", llama_token_to_str(ctx, embd[i]));
                //}
                //printf("'\n");
                //printf("\n---\n");
            }

            // try to reuse a matching prefix from the loaded session instead of re-eval (via n_past)
            if (n_session_consumed < (int) session_tokens.size()) {
                size_t i = 0;
                for ( ; i < embd.size(); i++) {
                    if (embd[i] != session_tokens[n_session_consumed]) {
                        session_tokens.resize(n_session_consumed);
                        break;
                    }

                    n_past++;
                    n_session_consumed++;

                    if (n_session_consumed >= (int) session_tokens.size()) {
                        ++i;
                        break;
                    }
                }
                if (i > 0) {
                    embd.erase(embd.begin(), embd.begin() + i);
                }
            }

            // evaluate tokens in batches
            // embd is typically prepared beforehand to fit within a batch, but not always
            for (int i = 0; i < (int) embd.size(); i += params.n_batch) {
                int n_eval = (int) embd.size() - i;
                if (n_eval > params.n_batch) {
                    n_eval = params.n_batch;
                }
                if (llama_eval(ctx, &embd[i], n_eval, n_past, params.n_threads)) {
                    fprintf(stderr, "%s : failed to eval\n", __func__);
                    return 1;
                }
                n_past += n_eval;
            }

            if (embd.size() > 0 && !path_session.empty()) {
                session_tokens.insert(session_tokens.end(), embd.begin(), embd.end());
                n_session_consumed = session_tokens.size();
            }
        }

        embd.clear();

        if ((int) embd_inp.size() <= n_consumed && !is_interacting) {
            // out of user input, sample next token
            const float   temp            = params.temp;
            const int32_t top_k           = params.top_k <= 0 ? llama_n_vocab(ctx) : params.top_k;
            const float   top_p           = params.top_p;
            const float   tfs_z           = params.tfs_z;
            const float   typical_p       = params.typical_p;
            const int32_t repeat_last_n   = params.repeat_last_n < 0 ? n_ctx : params.repeat_last_n;
            const float   repeat_penalty  = params.repeat_penalty;
            const float   alpha_presence  = params.presence_penalty;
            const float   alpha_frequency = params.frequency_penalty;
            const int     mirostat        = params.mirostat;
            const float   mirostat_tau    = params.mirostat_tau;
            const float   mirostat_eta    = params.mirostat_eta;
            const bool    penalize_nl     = params.penalize_nl;

            // optionally save the session on first sample (for faster prompt loading next time)
            if (!path_session.empty() && need_to_save_session && !params.prompt_cache_ro) {
                need_to_save_session = false;
                llama_save_session_file(ctx, path_session.c_str(), session_tokens.data(), session_tokens.size());
            }

            llama_token id = 0;

            {
                auto logits  = llama_get_logits(ctx);
                auto n_vocab = llama_n_vocab(ctx);

                // Apply params.logit_bias map
                for (auto it = params.logit_bias.begin(); it != params.logit_bias.end(); it++) {
                    logits[it->first] += it->second;
                }

                std::vector<llama_token_data> candidates;
                candidates.reserve(n_vocab);
                for (llama_token token_id = 0; token_id < n_vocab; token_id++) {
                    candidates.emplace_back(llama_token_data{token_id, logits[token_id], 0.0f});
                }

                llama_token_data_array candidates_p = { candidates.data(), candidates.size(), false };

                // Apply penalties
                float nl_logit = logits[llama_token_nl()];
                auto last_n_repeat = std::min(std::min((int)last_n_tokens.size(), repeat_last_n), n_ctx);
                llama_sample_repetition_penalty(ctx, &candidates_p,
                    last_n_tokens.data() + last_n_tokens.size() - last_n_repeat,
                    last_n_repeat, repeat_penalty);
                llama_sample_frequency_and_presence_penalties(ctx, &candidates_p,
                    last_n_tokens.data() + last_n_tokens.size() - last_n_repeat,
                    last_n_repeat, alpha_frequency, alpha_presence);
                if (!penalize_nl) {
                    logits[llama_token_nl()] = nl_logit;
                }

                if (temp <= 0) {
                    // Greedy sampling
                    id = llama_sample_token_greedy(ctx, &candidates_p);
                } else {
                    if (mirostat == 1) {
                        static float mirostat_mu = 2.0f * mirostat_tau;
                        const int mirostat_m = 100;
                        llama_sample_temperature(ctx, &candidates_p, temp);
                        id = llama_sample_token_mirostat(ctx, &candidates_p, mirostat_tau, mirostat_eta, mirostat_m, &mirostat_mu);
                    } else if (mirostat == 2) {
                        static float mirostat_mu = 2.0f * mirostat_tau;
                        llama_sample_temperature(ctx, &candidates_p, temp);
                        id = llama_sample_token_mirostat_v2(ctx, &candidates_p, mirostat_tau, mirostat_eta, &mirostat_mu);
                    } else {
                        // Temperature sampling
                        llama_sample_top_k(ctx, &candidates_p, top_k, 1);
                        llama_sample_tail_free(ctx, &candidates_p, tfs_z, 1);
                        llama_sample_typical(ctx, &candidates_p, typical_p, 1);
                        llama_sample_top_p(ctx, &candidates_p, top_p, 1);
                        llama_sample_temperature(ctx, &candidates_p, temp);
                        id = llama_sample_token(ctx, &candidates_p);
                    }
                }
                // printf("`%d`", candidates_p.size);

                last_n_tokens.erase(last_n_tokens.begin());
                last_n_tokens.push_back(id);
            }

            // replace end of text token with newline token when in interactive mode
            if (id == llama_token_eos() && params.interactive && !params.instruct) {
                id = llama_token_newline.front();
                if (params.antiprompt.size() != 0) {
                    // tokenize and inject first reverse prompt
                    const auto first_antiprompt = ::llama_tokenize(ctx, params.antiprompt.front(), false);
                    embd_inp.insert(embd_inp.end(), first_antiprompt.begin(), first_antiprompt.end());
                }
            }

            // add it to the context
            embd.push_back(id);

            // echo this to console
            input_echo = true;

            // decrement remaining sampling budget
            --n_remain;
        } else {
            // some user input remains from prompt or interaction, forward it to processing
            while ((int) embd_inp.size() > n_consumed) {
                embd.push_back(embd_inp[n_consumed]);
                last_n_tokens.erase(last_n_tokens.begin());
                last_n_tokens.push_back(embd_inp[n_consumed]);
                ++n_consumed;
                if ((int) embd.size() >= params.n_batch) {
                    break;
                }
            }
        }

        // display text
        if (input_echo) {
            for (auto id : embd) {
                std::string next_tokenized_string = llama_token_to_str(ctx, id);
                AI_answer_to_TTS += next_tokenized_string;
                printf("%s", next_tokenized_string.c_str());
            }
            fflush(stdout);
        }
        // reset color to default if we there is no pending user input
        if (input_echo && (int)embd_inp.size() == n_consumed) {
            console_set_color(con_st, CONSOLE_COLOR_DEFAULT);
        }

        // if not currently processing queued inputs;
        if ((int) embd_inp.size() <= n_consumed) {
            const bool                       debug_tts             = params.debug_tts;
            const bool                       enable_tts            = params.enable_tts;
            const std::string                tts_ignored_text      = params.tts_ignored_text;
            const bool                       enable_whisper        = params.enable_whisper;

            // check for reverse prompt
            if (params.antiprompt.size()) {
                std::string last_output;
                for (auto id : last_n_tokens) {
                    last_output += llama_token_to_str(ctx, id);
                }

                is_antiprompt = false;
                // Check if each of the reverse prompts appears at the end of the output.
                // If we're not running interactively, the reverse prompt might be tokenized with some following characters
                // so we'll compensate for that by widening the search window a bit.
                for (std::string & antiprompt : params.antiprompt) {
                    size_t extra_padding = params.interactive ? 0 : 2;
                    size_t search_start_pos = last_output.length() > static_cast<size_t>(antiprompt.length() + extra_padding)
                        ? last_output.length() - static_cast<size_t>(antiprompt.length() + extra_padding)
                        : 0;

                    if (last_output.find(antiprompt.c_str(), search_start_pos) != std::string::npos) {
                        if (params.interactive) {
                            is_interacting = true;
                            console_set_color(con_st, CONSOLE_COLOR_USER_INPUT);
                        }
                        is_antiprompt = true;
                        fflush(stdout);
                        break;
                    }
                }
            }

            if (n_past > 0 && is_interacting) {
                if(has_original_prompt_been_read)
                {
                    if(enable_tts)
                    {
                        std::vector<std::string> antiprompt = params.antiprompt;
                        std::string antiprompt_str = std::accumulate(antiprompt.begin(), antiprompt.end(), std::string(""));
                        // Replace tts_ignored_text and antiprompt from AI_answer_to_TTS with ""
                        AI_answer_to_TTS.replace(AI_answer_to_TTS.find(antiprompt_str), antiprompt_str.length(), "");
                        AI_answer_to_TTS.replace(AI_answer_to_TTS.find(tts_ignored_text), tts_ignored_text.length(), "");

                        // Sanitize AI_answer_to_TTS
                        AI_answer_to_TTS.erase(std::remove_if(AI_answer_to_TTS.begin(), AI_answer_to_TTS.end(), [](unsigned char c) {
                            return !(std::isalnum(c) || c == '?' || c == '!' || c == '.' || c == ',' || c == ' ' || c == '\'' || c == '-');
                        }), AI_answer_to_TTS.end());

                        start_tts(AI_answer_to_TTS, debug_tts);
                    }

                    if(enable_whisper) whisper_process_info = start_whisper();

                    AI_answer_to_TTS = "";
                }
                else
                {
                    //if(enable_tts) start_tts("I am ready to start", debug_tts);
                    if(enable_whisper) whisper_process_info = start_whisper();
                    has_original_prompt_been_read = true;
                    AI_answer_to_TTS = "";
                }

                if (params.instruct) {
                    printf("\n> ");
                }

                std::string buffer;
                if (!params.input_prefix.empty()) {
                    buffer += params.input_prefix;
                    printf("%s", buffer.c_str());
                }
                
                if(enable_whisper)
                {
                    DWORD bytesRead = 0;
                    char whisper_line_buffer[128] = { 0 };
                    bool another_line = true;

                    while(another_line && ReadFile(whisper_process_stdoutRead, whisper_line_buffer, sizeof(whisper_line_buffer) - 1, &bytesRead, NULL))
                    {
                        whisper_line_buffer[bytesRead] = '\0'; // Ensure null-termination
                        buffer.append(whisper_line_buffer, bytesRead);
                        another_line = !has_over_and_out_been_whispered(buffer);
                        std::cout << whisper_line_buffer;
                    }

                    stop_process(whisper_process_info);

                    if (con_st.out != stdout) {
                        fflush(stdout);
                    }

                    sanitize_buffer(buffer);

                    fflush(con_st.out); // Ensure all output is displayed before waiting for input
                }
                else
                {
                    std::string line;
                    bool another_line = true;
                    do {
                        another_line = console_readline(con_st, line);
                        buffer += line;
                    } while (another_line);
                }

                // done taking input, reset color
                console_set_color(con_st, CONSOLE_COLOR_DEFAULT);

                // Add tokens to embd only if the input buffer is non-empty
                // Entering a empty line lets the user pass control back
                if (buffer.length() > 1) {
                    // append input suffix if any
                    if (!params.input_suffix.empty()) {
                        buffer += params.input_suffix;
                        printf("%s", params.input_suffix.c_str());
                    }

                    // instruct mode: insert instruction prefix
                    if (params.instruct && !is_antiprompt) {
                        n_consumed = embd_inp.size();
                        embd_inp.insert(embd_inp.end(), inp_pfx.begin(), inp_pfx.end());
                    }

                    auto line_inp = ::llama_tokenize(ctx, buffer, false);
                    embd_inp.insert(embd_inp.end(), line_inp.begin(), line_inp.end());

                    // instruct mode: insert response suffix
                    if (params.instruct) {
                        embd_inp.insert(embd_inp.end(), inp_sfx.begin(), inp_sfx.end());
                    }

                    n_remain -= line_inp.size();
                }

                input_echo = false; // do not echo this again
            }

            if (n_past > 0) {
                is_interacting = false;
            }
        }

        // end of text token
        if (!embd.empty() && embd.back() == llama_token_eos()) {
            if (params.instruct) {
                is_interacting = true;
            } else {
                fprintf(stderr, " [end of text]\n");
                break;
            }
        }

        // In interactive mode, respect the maximum number of tokens and drop back to user input when reached.
        if (params.interactive && n_remain <= 0 && params.n_predict != -1) {
            n_remain = params.n_predict;
            is_interacting = true;
        }
    }

    if (!path_session.empty() && params.prompt_cache_all && !params.prompt_cache_ro) {
        fprintf(stderr, "\n%s: saving final output to session file '%s'\n", __func__, path_session.c_str());
        llama_save_session_file(ctx, path_session.c_str(), session_tokens.data(), session_tokens.size());
    }

    stop_process(whisper_process_info);
    llama_print_timings(ctx);
    llama_free(ctx);
    llama_free_model(model);

    return 0;
}
