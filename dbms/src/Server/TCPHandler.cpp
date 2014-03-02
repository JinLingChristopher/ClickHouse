#include <iomanip>

#include <boost/bind.hpp>

#include <Poco/Net/NetException.h>
#include <Poco/Ext/ScopedTry.h>

#include <Yandex/Revision.h>

#include <statdaemons/Stopwatch.h>

#include <DB/Core/ErrorCodes.h>
#include <DB/Core/Progress.h>

#include <DB/IO/CompressedReadBuffer.h>
#include <DB/IO/CompressedWriteBuffer.h>
#include <DB/IO/ReadBufferFromPocoSocket.h>
#include <DB/IO/WriteBufferFromPocoSocket.h>

#include <DB/IO/copyData.h>

#include <DB/DataStreams/AsynchronousBlockInputStream.h>
#include <DB/Interpreters/executeQuery.h>

#include "TCPHandler.h"


namespace DB
{


void TCPHandler::runImpl()
{
	connection_context = *server.global_context;
	connection_context.setSessionContext(connection_context);

	Settings global_settings = server.global_context->getSettings();

	socket().setReceiveTimeout(global_settings.receive_timeout);
	socket().setSendTimeout(global_settings.send_timeout);
	
	in = new ReadBufferFromPocoSocket(socket());
	out = new WriteBufferFromPocoSocket(socket());

	try
	{
		receiveHello();
	}
	catch (const Exception & e)	/// Типично при неправильном имени пользователя, пароле, адресе.
	{
		if (e.code() == ErrorCodes::CLIENT_HAS_CONNECTED_TO_WRONG_PORT)
		{
			LOG_DEBUG(log, "Client has connected to wrong port.");
			return;
		}

		try
		{
			/// Пытаемся отправить информацию об ошибке клиенту.
			sendException(e);
		}
		catch (...) {}

		throw;
	}

	/// При соединении может быть указана БД по-умолчанию.
	if (!default_database.empty())
	{
		if (!connection_context.isDatabaseExist(default_database))
		{
			Exception e("Database " + default_database + " doesn't exist", ErrorCodes::UNKNOWN_DATABASE);
			LOG_ERROR(log, "Code: " << e.code() << ", e.displayText() = " << e.displayText()
				<< ", Stack trace:\n\n" << e.getStackTrace().toString());
			sendException(e);
			return;
		}

		connection_context.setCurrentDatabase(default_database);
	}
	
	sendHello();

	connection_context.setProgressCallback(boost::bind(&TCPHandler::updateProgress, this, _1, _2));

	while (1)
	{
		/// Ждём пакета от клиента. При этом, каждые POLL_INTERVAL сек. проверяем, не требуется ли завершить работу.
		while (!static_cast<ReadBufferFromPocoSocket &>(*in).poll(global_settings.poll_interval * 1000000) && !Daemon::instance().isCancelled())
			;

		/// Если требуется завершить работу, или клиент отсоединился.
		if (Daemon::instance().isCancelled() || in->eof())
			break;
		
		Stopwatch watch;
		state.reset();

		/** Исключение во время выполнения запроса (его надо отдать по сети клиенту).
		  * Клиент сможет его принять, если оно не произошло во время отправки другого пакета и клиент ещё не разорвал соединение.
		  */
		SharedPtr<Exception> exception;
		
		try
		{
			/// Восстанавливаем контекст запроса.
			query_context = connection_context;

			/** Если Query - обрабатываем. Если Ping или Cancel - возвращаемся в начало.
			  * Могут прийти настройки на отдельный запрос, которые модифицируют query_context.
			  */
			if (!receivePacket())
				continue;

			/// Обрабатываем Query
			
			after_check_cancelled.restart();
			after_send_progress.restart();

			/// Запрос требует приёма данных от клиента?
			if (state.io.out)
				processInsertQuery(global_settings);
			else
				processOrdinaryQuery();

			sendEndOfStream();

			state.reset();
		}
		catch (const Exception & e)
		{
			LOG_ERROR(log, "Code: " << e.code() << ", e.displayText() = " << e.displayText() << ", e.what() = " << e.what()
				<< ", Stack trace:\n\n" << e.getStackTrace().toString());
			exception = e.clone();

			if (e.code() == ErrorCodes::UNKNOWN_PACKET_FROM_CLIENT)
				throw;
		}
		catch (const Poco::Net::NetException & e)
		{
			/** Сюда мы можем попадать, если была ошибка в соединении с клиентом,
			  *  или в соединении с удалённым сервером, который использовался для обработки запроса.
			  * Здесь не получается отличить эти два случая.
			  * Хотя в одном из них, мы должны отправить эксепшен клиенту, а в другом - не можем.
			  * Будем пытаться отправить эксепшен клиенту в любом случае - см. ниже.
			  */
			LOG_ERROR(log, "Poco::Net::NetException. Code: " << ErrorCodes::POCO_EXCEPTION << ", e.code() = " << e.code()
				<< ", e.displayText() = " << e.displayText() << ", e.what() = " << e.what());
			exception = new Exception(e.displayText(), e.code());
		}
		catch (const Poco::Exception & e)
		{
			LOG_ERROR(log, "Poco::Exception. Code: " << ErrorCodes::POCO_EXCEPTION << ", e.code() = " << e.code()
				<< ", e.displayText() = " << e.displayText() << ", e.what() = " << e.what());
			exception = new Exception(e.displayText(), e.code());
		}
		catch (const std::exception & e)
		{
			LOG_ERROR(log, "std::exception. Code: " << ErrorCodes::STD_EXCEPTION << ", e.what() = " << e.what());
			exception = new Exception(e.what(), ErrorCodes::STD_EXCEPTION);
		}
		catch (...)
		{
			LOG_ERROR(log, "Unknown exception. Code: " << ErrorCodes::UNKNOWN_EXCEPTION);
			exception = new Exception("Unknown exception", ErrorCodes::UNKNOWN_EXCEPTION);
		}

		bool network_error = false;

		try
		{
			if (exception)
				sendException(*exception);
		}
		catch (...)
		{
			/** Не удалось отправить информацию об эксепшене клиенту. */
			network_error = true;
			LOG_WARNING(log, "Client has gone away.");
		}

		try
		{
			state.reset();
		}
		catch (...)
		{
			/** В процессе обработки запроса было исключение, которое мы поймали и, возможно, отправили клиенту.
			  * При уничтожении конвейера выполнения запроса, было второе исключение.
			  * Например, конвейер мог выполняться в нескольких потоках, и в каждом из них могло возникнуть исключение.
			  * Проигнорируем его.
			  */
		}

		watch.stop();

		LOG_INFO(log, std::fixed << std::setprecision(3)
			<< "Processed in " << watch.elapsedSeconds() << " sec.");

		if (network_error)
			break;
	}
}


void TCPHandler::processInsertQuery(const Settings & global_settings)
{
	/// Отправляем клиенту блок - структура таблицы.
	Block block = state.io.out_sample;
	sendData(block);

	state.io.out->writePrefix();
	while (1)
	{
		/// Ждём пакета от клиента. При этом, каждые POLL_INTERVAL сек. проверяем, не требуется ли завершить работу.
		while (!static_cast<ReadBufferFromPocoSocket &>(*in).poll(global_settings.poll_interval * 1000000) && !Daemon::instance().isCancelled())
			;

		/// Если требуется завершить работу, или клиент отсоединился.
		if (Daemon::instance().isCancelled() || in->eof())
			return;

		if (!receivePacket())
			break;
	}
	state.io.out->writeSuffix();
}


void TCPHandler::processOrdinaryQuery()
{
	/// Вынимаем результат выполнения запроса, если есть, и пишем его в сеть.
	if (state.io.in)
	{
		/// Отправим блок-заголовок, чтобы клиент мог подготовить формат вывода
		if (state.io.in_sample && client_revision >= DBMS_MIN_REVISION_WITH_HEADER_BLOCK)
			sendData(state.io.in_sample);

		AsynchronousBlockInputStream async_in(state.io.in);
		async_in.readPrefix();

		std::stringstream query_pipeline;
		async_in.dumpTree(query_pipeline);
		LOG_DEBUG(log, "Query pipeline:\n" << query_pipeline.rdbuf());

		Stopwatch watch;
		while (true)
		{
			Block block;
			
			while (true)
			{
				if (isQueryCancelled())
				{
					/// Получен пакет с просьбой прекратить выполнение запроса.
					async_in.cancel();
					break;
				}
				else
				{
					if (state.rows_processed && after_send_progress.elapsed() / 1000 >= query_context.getSettingsRef().interactive_delay)
					{
						/// Прошло некоторое время и есть прогресс.
						after_send_progress.restart();
						sendProgress();
					}
				
					if (async_in.poll(query_context.getSettingsRef().interactive_delay / 1000))
					{
						/// Есть следующий блок результата.
						block = async_in.read();
						break;
					}
				}
			}

			/// Если закончились данные, то отправим данные профайлинга и тотальные значения до
			/// последнего нулевого блока, чтобы иметь возможность использовать
			/// эту информацию в выводе суффикса output stream'а
			if (!block)
			{
				sendTotals();
				sendExtremes();
				sendProfileInfo();
				sendProgress();
			}
			
			sendData(block);			
			if (!block)
				break;
		}

		async_in.readSuffix();

		watch.stop();
		logProfileInfo(watch, *state.io.in);
	}
}


void TCPHandler::sendProfileInfo()
{
	if (client_revision < DBMS_MIN_REVISION_WITH_PROFILING_PACKET)
		return;

	if (const IProfilingBlockInputStream * input = dynamic_cast<const IProfilingBlockInputStream *>(&*state.io.in))
	{
		writeVarUInt(Protocol::Server::ProfileInfo, *out);
		input->getInfo().write(*out);
		out->next();
	}
}


void TCPHandler::sendTotals()
{
	if (client_revision < DBMS_MIN_REVISION_WITH_TOTALS_EXTREMES)
		return;

	if (IProfilingBlockInputStream * input = dynamic_cast<IProfilingBlockInputStream *>(&*state.io.in))
	{
		const Block & totals = input->getTotals();

		if (totals)
		{
			initBlockOutput();

			writeVarUInt(Protocol::Server::Totals, *out);

			state.block_out->write(totals);
			state.maybe_compressed_out->next();
			out->next();
		}
	}
}


void TCPHandler::sendExtremes()
{
	if (client_revision < DBMS_MIN_REVISION_WITH_TOTALS_EXTREMES)
		return;

	if (const IProfilingBlockInputStream * input = dynamic_cast<const IProfilingBlockInputStream *>(&*state.io.in))
	{
		const Block & extremes = input->getExtremes();

		if (extremes)
		{
			initBlockOutput();

			writeVarUInt(Protocol::Server::Extremes, *out);

			state.block_out->write(extremes);
			state.maybe_compressed_out->next();
			out->next();
		}
	}
}


void TCPHandler::logProfileInfo(Stopwatch & watch, IBlockInputStream & in)
{
	/// Выведем информацию о том, сколько считано строк и байт.
	size_t rows = 0;
	size_t bytes = 0;

	in.getLeafRowsBytes(rows, bytes);

	if (rows != 0)
	{
		LOG_INFO(log, std::fixed << std::setprecision(3)
			<< "Read " << rows << " rows, " << bytes / 1048576.0 << " MiB in " << watch.elapsedSeconds() << " sec., "
			<< static_cast<size_t>(rows / watch.elapsedSeconds()) << " rows/sec., " << bytes / 1048576.0 / watch.elapsedSeconds() << " MiB/sec.");
	}

	QuotaForIntervals & quota = query_context.getQuota();
	if (!quota.empty())
		LOG_INFO(log, "Quota:\n" << quota.toString());
}


void TCPHandler::receiveHello()
{
	/// Получить hello пакет.
	UInt64 packet_type = 0;
	String client_name;
	UInt64 client_version_major = 0;
	UInt64 client_version_minor = 0;
	String user = "default";
	String password;

	readVarUInt(packet_type, *in);
	if (packet_type != Protocol::Client::Hello)
	{
		/** Если случайно обратились по протоколу HTTP на порт, предназначенный для внутреннего TCP-протокола,
		  *  то вместо номера пакета будет G (GET) или P (POST), в большинстве случаев.
		  */
		if (packet_type == 'G' || packet_type == 'P')
		{
			writeString("HTTP/1.0 400 Bad Request\r\n\r\n"
				"Port " + server.config.getString("tcp_port") + " is for clickhouse-client program.\r\n"
				"You must use port " + server.config.getString("http_port") + " for HTTP"
				+ (server.config.getBool("use_olap_http_server", false)
					? "\r\n or port " + server.config.getString("olap_http_port") + " for OLAPServer compatibility layer.\r\n"
					: ".\r\n"),
				*out);

			throw Exception("Client has connected to wrong port", ErrorCodes::CLIENT_HAS_CONNECTED_TO_WRONG_PORT);
		}
		else
			throw Exception("Unexpected packet from client", ErrorCodes::UNEXPECTED_PACKET_FROM_CLIENT);
	}

	readStringBinary(client_name, *in);
	readVarUInt(client_version_major, *in);
	readVarUInt(client_version_minor, *in);
	readVarUInt(client_revision, *in);
	readStringBinary(default_database, *in);

	if (client_revision >= DBMS_MIN_REVISION_WITH_USER_PASSWORD)
	{
		readStringBinary(user, *in);
		readStringBinary(password, *in);
	}

	LOG_DEBUG(log, "Connected " << client_name
		<< " version " << client_version_major
		<< "." << client_version_minor
		<< "." << client_revision
		<< (!default_database.empty() ? ", database: " + default_database : "")
		<< (!user.empty() ? ", user: " + user : "")
		<< ".");

	connection_context.setUser(user, password, socket().peerAddress().host(), "");
}


void TCPHandler::sendHello()
{
	writeVarUInt(Protocol::Server::Hello, *out);
	writeStringBinary(DBMS_NAME, *out);
	writeVarUInt(DBMS_VERSION_MAJOR, *out);
	writeVarUInt(DBMS_VERSION_MINOR, *out);
	writeVarUInt(Revision::get(), *out);
	out->next();
}


bool TCPHandler::receivePacket()
{
	UInt64 packet_type = 0;
	readVarUInt(packet_type, *in);

//	std::cerr << "Packet: " << packet_type << std::endl;

	switch (packet_type)
	{
		case Protocol::Client::Query:
			if (!state.empty())
				throw Exception("Unexpected packet Query received from client", ErrorCodes::UNEXPECTED_PACKET_FROM_CLIENT);
			receiveQuery();
			return true;

		case Protocol::Client::Data:
			if (state.empty())
				throw Exception("Unexpected packet Data received from client", ErrorCodes::UNEXPECTED_PACKET_FROM_CLIENT);
			return receiveData();

		case Protocol::Client::Ping:
			writeVarUInt(Protocol::Server::Pong, *out);
			out->next();
			return false;

		case Protocol::Client::Cancel:
			return false;

		case Protocol::Client::Hello:
			throw Exception("Unexpected packet " + String(Protocol::Client::toString(packet_type)) + " received from client",
				ErrorCodes::UNEXPECTED_PACKET_FROM_CLIENT);

		default:
			throw Exception("Unknown packet from client", ErrorCodes::UNKNOWN_PACKET_FROM_CLIENT);
	}
}


void TCPHandler::receiveQuery()
{
	UInt64 stage = 0;
	UInt64 compression = 0;

	state.is_empty = false;
	if (client_revision < DBMS_MIN_REVISION_WITH_STRING_QUERY_ID)
	{
		UInt64 query_id_int;
		readIntBinary(query_id_int, *in);
		state.query_id = "";
	}
	else
		readStringBinary(state.query_id, *in);

	query_context.setCurrentQueryId(state.query_id);

	/// Настройки на отдельный запрос.
	if (client_revision >= DBMS_MIN_REVISION_WITH_PER_QUERY_SETTINGS)
	{
		query_context.getSettingsRef().deserialize(*in);
	}

	readVarUInt(stage, *in);
	state.stage = QueryProcessingStage::Enum(stage);

	readVarUInt(compression, *in);
	state.compression = Protocol::Compression::Enum(compression);

	readStringBinary(state.query, *in);

	LOG_DEBUG(log, "Query ID: " << state.query_id);
	LOG_DEBUG(log, "Query: " << state.query);
	LOG_DEBUG(log, "Requested stage: " << QueryProcessingStage::toString(stage));

	state.io = executeQuery(state.query, query_context, false, state.stage);
}


bool TCPHandler::receiveData()
{
	initBlockInput();
	
	/// Прочитать из сети один блок и засунуть его в state.io.out (данные для INSERT-а)
	Block block = state.block_in->read();
	if (block)
	{
		state.io.out->write(block);
		return true;
	}
	else
		return false;
}


void TCPHandler::initBlockInput()
{
	if (!state.block_in)
	{
		if (state.compression == Protocol::Compression::Enable)
			state.maybe_compressed_in = new CompressedReadBuffer(*in);
		else
			state.maybe_compressed_in = in;

		state.block_in = query_context.getFormatFactory().getInput(
			"Native",
			*state.maybe_compressed_in,
			state.io.out_sample,
			query_context.getSettingsRef().max_block_size,
			query_context.getDataTypeFactory());
	}
}


void TCPHandler::initBlockOutput()
{
	if (!state.block_out)
	{
		if (state.compression == Protocol::Compression::Enable)
			state.maybe_compressed_out = new CompressedWriteBuffer(*out);
		else
			state.maybe_compressed_out = out;

		state.block_out = query_context.getFormatFactory().getOutput(
			"Native",
			*state.maybe_compressed_out,
			state.io.in_sample);
	}
}


bool TCPHandler::isQueryCancelled()
{
	if (state.is_cancelled || state.sent_all_data)
		return true;

	if (after_check_cancelled.elapsed() / 1000 < query_context.getSettingsRef().interactive_delay)
		return false;

	after_check_cancelled.restart();

	/// Во время выполнения запроса, единственный пакет, который может прийти от клиента - это остановка выполнения запроса.
	if (static_cast<ReadBufferFromPocoSocket &>(*in).poll(0))
	{
		UInt64 packet_type = 0;
		readVarUInt(packet_type, *in);

		switch (packet_type)
		{
			case Protocol::Client::Cancel:
				if (state.empty())
					throw Exception("Unexpected packet Cancel received from client", ErrorCodes::UNEXPECTED_PACKET_FROM_CLIENT);
				LOG_INFO(log, "Query was cancelled.");
				state.is_cancelled = true;
				return true;

			default:
				throw Exception("Unknown packet from client", ErrorCodes::UNKNOWN_PACKET_FROM_CLIENT);
		}
	}

	return false;
}


void TCPHandler::sendData(Block & block)
{
	initBlockOutput();

	writeVarUInt(Protocol::Server::Data, *out);

	state.block_out->write(block);
	state.maybe_compressed_out->next();
	out->next();
}


void TCPHandler::sendException(const Exception & e)
{
	writeVarUInt(Protocol::Server::Exception, *out);
	writeException(e, *out);
	out->next();
}


void TCPHandler::sendEndOfStream()
{
	state.sent_all_data = true;
	writeVarUInt(Protocol::Server::EndOfStream, *out);
	out->next();
}


void TCPHandler::updateProgress(size_t rows, size_t bytes)
{
	__sync_fetch_and_add(&state.rows_processed, rows);
	__sync_fetch_and_add(&state.bytes_processed, bytes);
}


void TCPHandler::sendProgress()
{
	size_t rows_processed = __sync_fetch_and_and(&state.rows_processed, 0);
	size_t bytes_processed = __sync_fetch_and_and(&state.bytes_processed, 0);

	writeVarUInt(Protocol::Server::Progress, *out);
	Progress progress(rows_processed, bytes_processed);
	progress.write(*out);
	out->next();
}


void TCPHandler::run()
{
	try
	{
		runImpl();

		LOG_INFO(log, "Done processing connection.");
	}
	catch (Exception & e)
	{
		LOG_ERROR(log, "Code: " << e.code() << ", e.displayText() = " << e.displayText()
			<< ", Stack trace:\n\n" << e.getStackTrace().toString());
	}
	catch (Poco::Exception & e)
	{
		std::stringstream message;
		message << "Poco::Exception. Code: " << ErrorCodes::POCO_EXCEPTION << ", e.code() = " << e.code()
			<< ", e.displayText() = " << e.displayText() << ", e.what() = " << e.what();

		/// Таймаут - не ошибка.
		if (!strcmp(e.what(), "Timeout"))
		{
			LOG_DEBUG(log, message.rdbuf());
		}
		else
		{
			LOG_ERROR(log, message.rdbuf());
		}
	}
	catch (std::exception & e)
	{
		LOG_ERROR(log, "std::exception. Code: " << ErrorCodes::STD_EXCEPTION << ". " << e.what());
	}
	catch (...)
	{
		LOG_ERROR(log, "Unknown exception. Code: " << ErrorCodes::UNKNOWN_EXCEPTION << ".");
	}
}


}
