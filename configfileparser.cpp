/*
This file is part of FlashMQ (https://www.flashmq.org)
Copyright (C) 2021 Wiebe Cazemier

FlashMQ is free software: you can redistribute it and/or modify
it under the terms of the GNU Affero General Public License as
published by the Free Software Foundation, version 3.

FlashMQ is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Affero General Public License for more details.

You should have received a copy of the GNU Affero General Public
License along with FlashMQ. If not, see <https://www.gnu.org/licenses/>.
*/

#include "configfileparser.h"

#include <fcntl.h>
#include <unistd.h>
#include <sstream>
#include <fstream>
#include <regex>
#include <sys/stat.h>

#include <openssl/ssl.h>
#include <openssl/err.h>

#include "exceptions.h"
#include "utils.h"


/**
 * @brief ConfigFileParser::testKeyValidity tests if two strings match and whether it's a valid config key.
 * @param key
 * @param matchKey
 * @param validKeys
 * @return
 *
 * Use of this function prevents adding config keys that you forget to add to the sets with valid keys.
 */
bool ConfigFileParser::testKeyValidity(const std::string &key, const std::string &matchKey, const std::set<std::string> &validKeys) const
{
    auto valid_key_it = validKeys.find(key);
    if (valid_key_it == validKeys.end())
    {
        std::ostringstream oss;
        oss << "Config key '" << key << "' is not valid (here).";
        throw ConfigFileException(oss.str());
    }

    {
        auto valid_key_it = validKeys.find(matchKey);
        if (valid_key_it == validKeys.end())
        {
            std::ostringstream oss;
            oss << "BUG: you still need to add '" << matchKey << "' as valid config key.";
            throw ConfigFileException(oss.str());
        }
    }

    return key == matchKey;
}

void ConfigFileParser::checkFileExistsAndReadable(const std::string &key, const std::string &pathToCheck, ssize_t max_size) const
{
    if (access(pathToCheck.c_str(), R_OK) != 0)
    {
        std::ostringstream oss;
        oss << "Error for '" << key << "': " << pathToCheck << " is not there or not readable";
        throw ConfigFileException(oss.str());
    }

    struct stat statbuf;
    memset(&statbuf, 0, sizeof(struct stat));
    if (stat(path.c_str(), &statbuf) < 0)
        throw ConfigFileException(formatString("Reading stat of '%s' failed.", pathToCheck.c_str()));

    if (!S_ISREG(statbuf.st_mode))
    {
        throw ConfigFileException(formatString("Error for '%s': '%s' is not a regular file.", key.c_str(), pathToCheck.c_str()));
    }

    if (statbuf.st_size > max_size)
    {
        throw ConfigFileException(formatString("Error for '%s': '%s' is bigger than %ld bytes.", key.c_str(), pathToCheck.c_str(), max_size));
    }
}

void ConfigFileParser::checkFileOrItsDirWritable(const std::string &filepath) const
{
    if (access(filepath.c_str(), F_OK) == 0)
    {
        if (access(filepath.c_str(), W_OK) != 0)
        {
            std::string msg = formatString("File '%s' is there, but not writable", filepath.c_str());
            throw ConfigFileException(msg);
        }
        return;
    }
    std::string dirname = dirnameOf(filepath);

    if (access(dirname.c_str(), W_OK) != 0)
    {
        std::string msg = formatString("File '%s' is not there and can't be created, because '%s' is also not writable", filepath.c_str(), dirname.c_str());
        throw ConfigFileException(msg);
    }
}

ConfigFileParser::ConfigFileParser(const std::string &path) :
    path(path)
{
    validKeys.insert("plugin");
    validKeys.insert("plugin_serialize_init");
    validKeys.insert("plugin_serialize_auth_checks");
    validKeys.insert("plugin_timer_period");
    validKeys.insert("log_file");
    validKeys.insert("quiet");
    validKeys.insert("allow_unsafe_clientid_chars");
    validKeys.insert("allow_unsafe_username_chars");
    validKeys.insert("client_initial_buffer_size");
    validKeys.insert("max_packet_size");
    validKeys.insert("log_debug");
    validKeys.insert("log_subscriptions");
    validKeys.insert("mosquitto_password_file");
    validKeys.insert("mosquitto_acl_file");
    validKeys.insert("allow_anonymous");
    validKeys.insert("rlimit_nofile");
    validKeys.insert("expire_sessions_after_seconds");
    validKeys.insert("thread_count");
    validKeys.insert("storage_dir");
    validKeys.insert("max_qos_msg_pending_per_client");
    validKeys.insert("max_qos_bytes_pending_per_client");
    validKeys.insert("wills_enabled");
    validKeys.insert("retained_messages_mode");
    validKeys.insert("expire_retained_messages_after_seconds");
    validKeys.insert("expire_retained_messages_time_budget_ms");
    validKeys.insert("websocket_set_real_ip_from");
    validKeys.insert("shared_subscription_targeting");
    validKeys.insert("max_incoming_topic_alias_value");
    validKeys.insert("max_outgoing_topic_alias_value");

    validListenKeys.insert("port");
    validListenKeys.insert("protocol");
    validListenKeys.insert("fullchain");
    validListenKeys.insert("privkey");
    validListenKeys.insert("inet_protocol");
    validListenKeys.insert("inet4_bind_address");
    validListenKeys.insert("inet6_bind_address");
    validListenKeys.insert("haproxy");
}

void ConfigFileParser::loadFile(bool test)
{
    if (path.empty())
        return;

    checkFileExistsAndReadable("application config file", path, 1024*1024*10);

    std::ifstream infile(path, std::ios::in);

    if (!infile.is_open())
    {
        std::ostringstream oss;
        oss << "Error loading " << path;
        throw ConfigFileException(oss.str());
    }

    std::list<std::string> lines;

    const std::regex key_value_regex("^([a-zA-Z0-9_\\-]+) +([a-zA-Z0-9_\\-/\\.:]+)$");
    const std::regex block_regex_start("^([a-zA-Z0-9_\\-]+) *\\{$");
    const std::regex block_regex_end("^\\}$");

    bool inBlock = false;
    std::ostringstream oss;
    int linenr = 0;

    // First parse the file and keep the valid lines.
    for(std::string line; getline(infile, line ); )
    {
        trim(line);
        linenr++;

        if (startsWith(line, "#"))
            continue;

        if (line.empty())
            continue;

        // The regex matcher can be made to crash on very long lines, so we're protecting ourselves.
        if (line.length() > 256)
        {
            throw ConfigFileException(formatString("Error at line %d in '%s': line suspiciouly long.", linenr, path.c_str()));
        }

        std::smatch matches;

        const bool blockStartMatch = std::regex_search(line, matches, block_regex_start);
        const bool blockEndMatch = std::regex_search(line, matches, block_regex_end);

        if ((blockStartMatch && inBlock) || (blockEndMatch && !inBlock))
        {
            oss << "Unexpected block start or end at line " << linenr << ": " << line;
            throw ConfigFileException(oss.str());
        }

        if (!std::regex_search(line, matches, key_value_regex) && !blockStartMatch && !blockEndMatch)
        {
            oss << "Line '" << line << "' invalid";
            throw ConfigFileException(oss.str());
        }

        if (blockStartMatch)
            inBlock = true;
        if (blockEndMatch)
            inBlock = false;

        lines.push_back(line);
    }

    if (inBlock)
    {
        throw ConfigFileException("Unclosed config block. Expecting }");
    }

    std::unordered_map<std::string, std::string> pluginOpts;

    ConfigParseLevel curParseLevel = ConfigParseLevel::Root;
    std::shared_ptr<Listener> curListener;
    Settings tmpSettings;

    // Then once we know the config file is valid, process it.
    for (std::string &line : lines)
    {
        std::smatch matches;

        if (std::regex_match(line, matches, block_regex_start))
        {
            std::string key = matches[1].str();
            if (matches[1].str() == "listen")
            {
                curParseLevel = ConfigParseLevel::Listen;
                curListener = std::make_shared<Listener>();
            }
            else
            {
                throw ConfigFileException(formatString("'%s' is not a valid block.", key.c_str()));
            }

            continue;
        }
        else if (std::regex_match(line, matches, block_regex_end))
        {
            if (curParseLevel == ConfigParseLevel::Listen)
            {
                curListener->isValid();
                tmpSettings.listeners.push_back(curListener);
                curListener.reset();
            }

            curParseLevel = ConfigParseLevel::Root;
            continue;
        }

        std::regex_match(line, matches, key_value_regex);

        std::string key = matches[1].str();
        const std::string value = matches[2].str();

        try
        {
            if (curParseLevel == ConfigParseLevel::Listen)
            {
                if (testKeyValidity(key, "protocol", validListenKeys))
                {
                    if (value != "mqtt" && value != "websockets")
                        throw ConfigFileException(formatString("Protocol '%s' is not a valid listener protocol", value.c_str()));
                    curListener->websocket = value == "websockets";
                }
                else if (testKeyValidity(key, "port", validListenKeys))
                {
                    curListener->port = std::stoi(value);
                }
                else if (testKeyValidity(key, "fullchain", validListenKeys))
                {
                    checkFileExistsAndReadable("SSL fullchain", value, 1024*1024);
                    curListener->sslFullchain = value;
                }
                if (testKeyValidity(key, "privkey", validListenKeys))
                {
                    checkFileExistsAndReadable("SSL privkey", value, 1024*1024);
                    curListener->sslPrivkey = value;
                }
                if (testKeyValidity(key, "inet_protocol", validListenKeys))
                {
                    if (value == "ip4")
                        curListener->protocol = ListenerProtocol::IPv4;
                    else if (value == "ip6")
                        curListener->protocol = ListenerProtocol::IPv6;
                    else if (value == "ip4_ip6")
                        curListener->protocol = ListenerProtocol::IPv46;
                    else
                        throw ConfigFileException(formatString("Invalid inet protocol: %s", value.c_str()));
                }
                if (testKeyValidity(key, "inet4_bind_address", validListenKeys))
                {
                    curListener->inet4BindAddress = value;
                }
                if (testKeyValidity(key, "inet6_bind_address", validListenKeys))
                {
                    curListener->inet6BindAddress = value;
                }
                if (testKeyValidity(key, "haproxy", validListenKeys))
                {
                    bool val = stringTruthiness(value);
                    curListener->haproxy = val;
                }

                continue;
            }


            const std::string plugin_opt_ = "plugin_opt_";
            if (startsWith(key, plugin_opt_))
            {
                key.replace(0, plugin_opt_.length(), "");
                pluginOpts[key] = value;
            }
            else
            {
                if (testKeyValidity(key, "plugin", validKeys))
                {
                    checkFileExistsAndReadable(key, value, 1024*1024*100);
                    tmpSettings.pluginPath = value;
                }

                if (testKeyValidity(key, "log_file", validKeys))
                {
                    checkFileOrItsDirWritable(value);
                    tmpSettings.logPath = value;
                }

                if (testKeyValidity(key, "quiet", validKeys))
                {
                    bool tmp = stringTruthiness(value);
                    tmpSettings.quiet = tmp;
                }

                if (testKeyValidity(key, "allow_unsafe_clientid_chars", validKeys))
                {
                    bool tmp = stringTruthiness(value);
                    tmpSettings.allowUnsafeClientidChars = tmp;
                }

                if (testKeyValidity(key, "allow_unsafe_username_chars", validKeys))
                {
                    bool tmp = stringTruthiness(value);
                    tmpSettings.allowUnsafeUsernameChars = tmp;
                }

                if (testKeyValidity(key, "plugin_serialize_init", validKeys))
                {
                    bool tmp = stringTruthiness(value);
                    tmpSettings.pluginSerializeInit = tmp;
                }

                if (testKeyValidity(key, "plugin_serialize_auth_checks", validKeys))
                {
                    bool tmp = stringTruthiness(value);
                    tmpSettings.pluginSerializeAuthChecks = tmp;
                }

                if (testKeyValidity(key, "client_initial_buffer_size", validKeys))
                {
                    int newVal = std::stoi(value);
                    if (!isPowerOfTwo(newVal))
                        throw ConfigFileException("client_initial_buffer_size value " + value + " is not a power of two.");
                    tmpSettings.clientInitialBufferSize = newVal;
                }

                if (testKeyValidity(key, "max_packet_size", validKeys))
                {
                    int newVal = std::stoi(value);
                    if (newVal > ABSOLUTE_MAX_PACKET_SIZE)
                    {
                        std::ostringstream oss;
                        oss << "Value for max_packet_size " << newVal << "is higher than absolute maximum " << ABSOLUTE_MAX_PACKET_SIZE;
                        throw ConfigFileException(oss.str());
                    }
                    tmpSettings.maxPacketSize = newVal;
                }

                if (testKeyValidity(key, "log_debug", validKeys))
                {
                    bool tmp = stringTruthiness(value);
                    tmpSettings.logDebug = tmp;
                }

                if (testKeyValidity(key, "log_subscriptions", validKeys))
                {
                    bool tmp = stringTruthiness(value);
                    tmpSettings.logSubscriptions = tmp;
                }

                if (testKeyValidity(key, "mosquitto_password_file", validKeys))
                {
                    checkFileExistsAndReadable("mosquitto_password_file", value, 1024*1024*1024);
                    tmpSettings.mosquittoPasswordFile = value;
                }

                if (testKeyValidity(key, "mosquitto_acl_file", validKeys))
                {
                    checkFileExistsAndReadable("mosquitto_acl_file", value, 1024*1024*1024);
                    tmpSettings.mosquittoAclFile = value;
                }

                if (testKeyValidity(key, "allow_anonymous", validKeys))
                {
                    bool tmp = stringTruthiness(value);
                    tmpSettings.allowAnonymous = tmp;
                }

                if (testKeyValidity(key, "rlimit_nofile", validKeys))
                {
                    int newVal = std::stoi(value);
                    if (newVal <= 0)
                    {
                        throw ConfigFileException(formatString("Value '%d' is negative.", newVal));
                    }
                    tmpSettings.rlimitNoFile = newVal;
                }

                if (testKeyValidity(key, "expire_sessions_after_seconds", validKeys))
                {
                    uint32_t newVal = std::stoi(value);
                    if (newVal > 0 && newVal < 60) // 0 means disable
                    {
                        throw ConfigFileException(formatString("expire_sessions_after_seconds value '%d' is invalid. Valid values are 0, or 60 or higher.", newVal));
                    }
                    tmpSettings.expireSessionsAfterSeconds = newVal;
                }

                if (testKeyValidity(key, "plugin_timer_period", validKeys))
                {
                    int newVal = std::stoi(value);
                    if (newVal < 0)
                    {
                        throw ConfigFileException(formatString("plugin_timer_period value '%d' is invalid. Valid values are 0 or higher. 0 means disabled.", newVal));
                    }
                    tmpSettings.pluginTimerPeriod = newVal;
                }

                if (testKeyValidity(key, "storage_dir", validKeys))
                {
                    std::string newPath = value;
                    rtrim(newPath, '/');
                    checkWritableDir<ConfigFileException>(newPath);
                    tmpSettings.storageDir = newPath;
                }

                if (testKeyValidity(key, "thread_count", validKeys))
                {
                    int newVal = std::stoi(value);
                    if (newVal < 0)
                    {
                        throw ConfigFileException(formatString("thread_count value '%d' is invalid. Valid values are 0 or higher. 0 means auto.", newVal));
                    }
                    tmpSettings.threadCount = newVal;
                }

                if (testKeyValidity(key, "max_qos_msg_pending_per_client", validKeys))
                {
                    int newVal = std::stoi(value);
                    if (newVal < 32 || newVal > 65535)
                    {
                        throw ConfigFileException(formatString("max_qos_msg_pending_per_client value '%d' is invalid. Valid values between 32 and 65535.", newVal));
                    }
                    tmpSettings.maxQosMsgPendingPerClient = newVal;
                }

                if (testKeyValidity(key, "max_qos_bytes_pending_per_client", validKeys))
                {
                    int newVal = std::stoi(value);
                    if (newVal < 4096)
                    {
                        throw ConfigFileException(formatString("max_qos_bytes_pending_per_client value '%d' is invalid. Valid values are 4096 or higher.", newVal));
                    }
                    tmpSettings.maxQosBytesPendingPerClient = newVal;
                }

                if (testKeyValidity(key, "max_incoming_topic_alias_value", validKeys))
                {
                    int newVal = std::stoi(value);
                    if (newVal < 0 || newVal > 0xFFFF)
                    {
                        throw ConfigFileException(formatString("max_incoming_topic_alias_value value '%d' is invalid. Valid values are between 0 and 65535.", newVal));
                    }
                    tmpSettings.maxIncomingTopicAliasValue = newVal;
                }

                if (testKeyValidity(key, "max_outgoing_topic_alias_value", validKeys))
                {
                    int newVal = std::stoi(value);
                    if (newVal < 0 || newVal > 0xFFFF)
                    {
                        throw ConfigFileException(formatString("max_outgoing_topic_alias_value value '%d' is invalid. Valid values are between 0 and 65535.", newVal));
                    }
                    tmpSettings.maxOutgoingTopicAliasValue = newVal;
                }

                if (testKeyValidity(key, "wills_enabled", validKeys))
                {
                    bool tmp = stringTruthiness(value);
                    tmpSettings.willsEnabled = tmp;
                }

                if (testKeyValidity(key, "retained_messages_mode", validKeys))
                {
                    const std::string _val = str_tolower(value);

                    if (_val == "enabled")
                        tmpSettings.retainedMessagesMode = RetainedMessagesMode::Enabled;
                    else if (_val == "downgrade")
                        tmpSettings.retainedMessagesMode = RetainedMessagesMode::Downgrade;
                    else if (_val == "drop")
                        tmpSettings.retainedMessagesMode = RetainedMessagesMode::Drop;
                    else if (_val == "disconnect_with_error")
                        tmpSettings.retainedMessagesMode = RetainedMessagesMode::DisconnectWithError;
                    else
                        throw ConfigFileException(formatString("Value '%s' for '%s' is invalid.", value.c_str(), key.c_str()));
                }

                if (testKeyValidity(key, "expire_retained_messages_after_seconds", validKeys))
                {
                    uint32_t newVal = std::stoi(value);
                    if (newVal < 1)
                    {
                        throw ConfigFileException(formatString("expire_retained_messages_after_seconds value '%d' is invalid. Valid values are between 1 and 4294967296.", newVal));
                    }
                    tmpSettings.expireRetainedMessagesAfterSeconds = newVal;
                }

                if (testKeyValidity(key, "expire_retained_messages_time_budget_ms", validKeys))
                {
                    uint32_t newVal = std::stoi(value);
                    tmpSettings.expireRetainedMessagesTimeBudgetMs = newVal;
                }

                if (testKeyValidity(key, "websocket_set_real_ip_from", validKeys))
                {
                    Network net(value);
                    tmpSettings.setRealIpFrom.push_back(std::move(net));
                }

                if (testKeyValidity(key, "shared_subscription_targeting", validKeys))
                {
                    const std::string _val = str_tolower(value);

                    if (_val == "round_robin")
                        tmpSettings.sharedSubscriptionTargeting = SharedSubscriptionTargeting::RoundRobin;
                    else if (_val == "sender_hash")
                        tmpSettings.sharedSubscriptionTargeting = SharedSubscriptionTargeting::SenderHash;
                    else
                        throw ConfigFileException(formatString("Value '%s' for '%s' is invalid.", value.c_str(), key.c_str()));
                }
            }
        }
        catch (std::invalid_argument &ex) // catch for the stoi()
        {
            throw ConfigFileException(ex.what());
        }
    }

    tmpSettings.authOptCompatWrap = AuthOptCompatWrap(pluginOpts);
    tmpSettings.flashmqpluginOpts = std::move(pluginOpts);

    if (!test)
    {
        this->settings = tmpSettings;
    }
}

const Settings &ConfigFileParser::getSettings()
{
    return settings;
}



