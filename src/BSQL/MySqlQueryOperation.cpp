#include "BSQL.h"

MySqlQueryOperation::MySqlQueryOperation(MySqlConnection& connPool, std::string&& queryText) :
	queryText(std::move(queryText)),
	connPool(connPool),
	connection(nullptr),
	state(std::make_shared<ClassState>()),
	connectionAttempts(0),
	started(false),
	complete(false),
	operationThread(TryStart())
{
}

MySqlQueryOperation::~MySqlQueryOperation() {
	if (!connection)
		return;
	connPool.ReleaseConnection(connection);
}

std::thread MySqlQueryOperation::TryStart() {
	connection = connPool.RequestConnection(error, errnum, noClose);
	if (!connection) {
		if (!error.empty())
			complete = ++connectionAttempts == 3;
		return std::thread();
	}
	started = true;
	return std::thread(&MySqlQueryOperation::StartQuery, this, connection, std::move(queryText), state);
}

void MySqlQueryOperation::QuestionableExit(MYSQL* mysql, std::shared_ptr<ClassState>& localClassState) {
	//resultless?
	localClassState->lock.lock();
	if (localClassState->alive) {
		complete = true;
		const auto tmpErr(mysql_errno(mysql));
		if (tmpErr) {
			//no it's an error
			error = mysql_error(mysql);
			errnum = tmpErr;
		}
	}
	else if (!noClose)
		mysql_close(mysql);
	mysql_thread_end();
	localClassState->lock.unlock();
}

void MySqlQueryOperation::StartQuery(MYSQL* mysql, std::string&& localQueryText, std::shared_ptr<ClassState> localClassState) {
	mysql_thread_init();

	const auto localError(mysql_real_query(mysql, localQueryText.c_str(), localQueryText.length()));

	if (localError) {
		QuestionableExit(mysql, localClassState);
		return;
	}

	const auto result(mysql_use_result(mysql));
	if (!result) {
		QuestionableExit(mysql, localClassState);
		return;
	}

	for (MYSQL_ROW row(mysql_fetch_row(result)); row != nullptr; row = mysql_fetch_row(result)) {
		try {
			std::string json("{");
			bool first(true);
			const auto numFields(mysql_num_fields(result));
			mysql_field_seek(result, 0);
			for (auto I(0U); I < numFields; ++I) {
				const auto field(mysql_fetch_field(result));
				if (first)
					first = false;
				else
					json.append(",");
				json.append("\"");
				json.append(field->name);
				json.append("\":");
				if (row[I] == nullptr)
					json.append("null");
				else {
					json.append("\"");
					json.append(row[I]);
					json.append("\"");
				}
			}
			json.append("}");

			localClassState->lock.lock();
			const auto alive(localClassState->alive);
			if (alive)
				results.emplace(std::move(json));
			localClassState->lock.unlock();
			if (!alive)
				break;
		}
		catch (std::bad_alloc&) {
			mysql_free_result(result);
			localClassState->lock.lock();
			if (localClassState->alive) {
				complete = true;
				errnum = -1;
				error = "Out of memory!";
			}
			else if (!noClose)
				mysql_close(mysql);
			mysql_thread_end();
			localClassState->lock.unlock();
			return;
		}
	}

	mysql_free_result(result);

	QuestionableExit(mysql, localClassState);
}

bool MySqlQueryOperation::IsComplete(bool noSkip) {
	if (!started) {
		operationThread = TryStart();
		return false;
	}

	state->lock.lock();
	if (!results.empty()) {
		if (!noSkip) {
			currentRow = std::move(results.front());
			results.pop();
		}
		state->lock.unlock();
		return true;
	}

	const auto result(complete);
	state->lock.unlock();
	if (result && !noSkip)
		currentRow = std::string();
	return result;
}

std::thread* MySqlQueryOperation::GetActiveThread() {
	if (!started)
		return nullptr;

	state->lock.lock();

	if (complete) {
		state->lock.unlock();
		operationThread.join();
		return nullptr;
	}

	state->alive = false;
	state->lock.unlock();
	connection = nullptr;
	return &operationThread;
}
