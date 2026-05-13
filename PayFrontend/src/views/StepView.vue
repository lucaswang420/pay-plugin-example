<template>
  <div class="step-view">
    <!-- 用户信息栏 -->
    <div v-if="userStore.isLoggedIn" class="user-info-bar">
      <span class="user-info">
        👤 用户ID: <strong>{{ userStore.userId }}</strong>
        | API密钥: <strong>{{ userStore.apiKey?.substring(0, 10) }}...</strong>
      </span>
      <button @click="handleLogout" class="logout-btn">退出登录</button>
    </div>

    <StepIndicator :current-step="parseInt(stepNumber)" />

    <div v-if="stepNumber === '1'">
      <h2>Create Order</h2>

      <form @submit.prevent="handleCreateOrder" class="order-form">
        <div class="form-group">
          <label for="productName">Product Name *</label>
          <input
            id="productName"
            v-model="form.productName"
            type="text"
            required
            placeholder="Enter product name"
          />
        </div>

        <div class="form-group">
          <label for="amount">Amount (¥) *</label>
          <input
            id="amount"
            v-model="form.amount"
            type="number"
            step="0.01"
            min="0.01"
            required
            placeholder="0.00"
          />
        </div>

        <div class="form-group">
          <label for="description">Description</label>
          <textarea
            id="description"
            v-model="form.description"
            rows="3"
            placeholder="Enter product description (optional)"
          ></textarea>
        </div>

        <div v-if="error" class="error-message">{{ error }}</div>

        <div class="button-group">
          <button type="submit" :disabled="loading">
            {{ loading ? 'Creating...' : 'Create Order & Generate QR Code' }}
          </button>
          <button type="button" @click="handleReset" class="secondary">
            Reset
          </button>
        </div>
      </form>
    </div>

    <div v-else-if="stepNumber === '2'">
      <h2>Payment Details</h2>
      <div class="order-details">
        <div class="detail-group">
          <label>Order Number:</label>
          <span class="detail-value">{{ orderStore.orderId }}</span>
        </div>

        <div class="detail-group">
          <label>Product Name:</label>
          <span class="detail-value">{{ orderStore.productName }}</span>
        </div>

        <div class="detail-group">
          <label>Amount:</label>
          <span class="detail-value amount">¥{{ orderStore.amount }}</span>
        </div>

        <div class="detail-group">
          <label>Description:</label>
          <span class="detail-value">{{ orderStore.description || 'N/A' }}</span>
        </div>

        <div class="detail-group">
          <label>Status:</label>
          <span :class="['status-badge', getStatusClass(orderStore.status)]">
            {{ orderStore.status || 'Unknown' }}
          </span>
        </div>

        <div v-if="orderStore.tradeNo" class="detail-group">
          <label>Alipay Trade No:</label>
          <span class="detail-value">{{ orderStore.tradeNo }}</span>
        </div>
      </div>

      <div class="button-group">
        <button type="button" @click="handleRefresh" :disabled="loading">
          {{ loading ? 'Refreshing...' : 'Refresh Status' }}
        </button>
        <button type="button" @click="handleBack" class="secondary">
          Create New Order
        </button>
      </div>

      <!-- 二维码显示区域 -->
      <div v-if="orderStore.status !== 'PAID'" class="qr-container">
        <div v-if="loading" class="qr-loading">Loading QR code...</div>
        <div v-else-if="error" class="error-message">{{ error }}</div>
        <div v-else-if="qrCodeUrl" class="qr-wrapper">
          <canvas ref="qrCanvas"></canvas>
          <p class="qr-instruction">
            Use Alipay Sandbox App to scan the QR code
          </p>
        </div>
        <div v-else class="qr-placeholder">
          No QR code available
        </div>
      </div>

      <!-- 支付成功提示 -->
      <div v-if="orderStore.status === 'PAID'" class="success-message">
        <div class="success-icon">✓</div>
        <h3>Payment Completed Successfully!</h3>
        <div class="success-details">
          <div class="detail-row">
            <span class="detail-label">Order No:</span>
            <span class="detail-value">{{ orderStore.orderId }}</span>
          </div>
          <div class="detail-row">
            <span class="detail-label">Trade No:</span>
            <span class="detail-value">{{ orderStore.tradeNo }}</span>
          </div>
          <div class="detail-row">
            <span class="detail-label">Amount:</span>
            <span class="detail-value amount">¥{{ orderStore.amount }}</span>
          </div>
          <div class="detail-row">
            <span class="detail-label">Payment Time:</span>
            <span class="detail-value">{{ paymentTime }}</span>
          </div>
        </div>
        <div class="button-group">
          <button
            type="button"
            @click="handleViewOrderDetail"
            class="primary"
          >
            View Order Details
          </button>
          <button
            type="button"
            @click="handleGoToRefund"
            class="warning"
          >
            Apply for Refund
          </button>
        </div>
      </div>

      <!-- 轮询状态提示 -->
      <div v-if="isPolling" class="polling-indicator">
        <span class="pulse"></span>
        Checking payment status automatically...
      </div>
    </div>

    <div v-else-if="stepNumber === '3'">
      <h2>My Orders</h2>

      <!-- 筛选栏 -->
      <div class="filter-bar">
        <div class="filter-group">
          <label>Status Filter:</label>
          <select v-model="orderStore.orderFilter" @change="handleFilterChange">
            <option value="all">All Orders</option>
            <option value="PENDING">Pending Payment</option>
            <option value="PAID">Paid</option>
            <option value="REFUNDED">Refunded</option>
            <option value="FAILED">Failed</option>
          </select>
        </div>

        <button @click="handleRefreshOrders" :disabled="listLoading" class="refresh-btn">
          {{ listLoading ? 'Syncing...' : 'Sync from Alipay' }}
        </button>
      </div>

      <!-- 订单列表 -->
      <div v-if="listLoading && !orderStore.orders.length" class="loading-message">
        Loading orders...
      </div>

      <div v-else-if="listError" class="error-message">{{ listError }}</div>

      <div v-else-if="!orderStore.orders.length" class="empty-message">
        <p>No orders found. Create your first order!</p>
        <button @click="handleBack" class="primary">Create Order</button>
      </div>

      <div v-else class="order-list">
        <div
          v-for="order in orderStore.orders"
          :key="order.order_no"
          class="order-card"
          :class="getStatusClass(order.status)"
        >
          <div class="order-card-header">
            <div class="order-no">{{ order.order_no }}</div>
            <span :class="['status-badge', getStatusClass(order.status)]">
              {{ formatStatus(order.status) }}
            </span>
          </div>

          <div class="order-card-body">
            <div class="order-info">
              <div class="info-row">
                <span class="label">Product:</span>
                <span class="value">{{ order.title || order.product_name || 'N/A' }}</span>
              </div>
              <div class="info-row">
                <span class="label">Amount:</span>
                <span class="value amount">¥{{ order.amount }}</span>
              </div>
              <div class="info-row" v-if="order.trade_no">
                <span class="label">Trade No:</span>
                <span class="value">{{ order.trade_no }}</span>
              </div>
              <div class="info-row">
                <span class="label">Created:</span>
                <span class="value">{{ formatDate(order.created_at) }}</span>
              </div>
            </div>

            <div class="order-actions">
              <button
                @click="handleViewOrderDetailFromList(order)"
                class="action-btn primary"
              >
                View Details
              </button>
              <button
                v-if="order.status === 'PAID'"
                @click="handleGoToRefundFromList(order)"
                class="action-btn warning"
              >
                Apply Refund
              </button>
            </div>
          </div>
        </div>
      </div>
    </div>

    <div v-else-if="stepNumber === '4'">
      <h2>Apply for Refund</h2>

      <!-- 订单信息预览 -->
      <div v-if="orderStore.currentOrder" class="order-preview">
        <h3>Order Information</h3>
        <div class="preview-row">
          <span class="label">Order No:</span>
          <span class="value">{{ orderStore.currentOrder.order_no }}</span>
        </div>
        <div class="preview-row">
          <span class="label">Product:</span>
          <span class="value">{{ orderStore.currentOrder.title || orderStore.currentOrder.product_name || 'N/A' }}</span>
        </div>
        <div class="preview-row">
          <span class="label">Amount:</span>
          <span class="value amount">¥{{ orderStore.currentOrder.amount }}</span>
        </div>
        <div class="preview-row">
          <span class="label">Status:</span>
          <span class="value">{{ formatStatus(orderStore.currentOrder.status) }}</span>
        </div>
      </div>

      <!-- 退款申请表单 -->
      <form @submit.prevent="handleSubmitRefund" class="refund-form">
        <div class="form-group">
          <label for="refundAmount">Refund Amount *</label>
          <input
            id="refundAmount"
            v-model.number="refundForm.amount"
            type="number"
            step="0.01"
            min="0.01"
            :max="orderStore.currentOrder?.amount || 0"
            required
          />
          <small class="form-hint">
            Full refund amount: ¥{{ orderStore.currentOrder?.amount || 0 }}
          </small>
        </div>

        <div class="form-group">
          <label for="refundReason">Refund Reason *</label>
          <select
            id="refundReason"
            v-model="refundForm.reason"
            required
          >
            <option value="">Please select a reason</option>
            <option value="不想要了">Don't want it anymore</option>
            <option value="商品质量问题">Product quality issue</option>
            <option value="发货太慢">Shipping too slow</option>
            <option value="买错了">Bought by mistake</option>
            <option value="其他">Other</option>
          </select>
        </div>

        <div v-if="listError" class="error-message">{{ listError }}</div>
        <div v-if="refundSuccess" class="success-message">{{ refundSuccess }}</div>

        <div class="button-group">
          <button type="submit" :disabled="refundLoading || !refundForm.reason">
            {{ refundLoading ? 'Processing...' : 'Submit Refund Request' }}
          </button>
          <button type="button" @click="handleCancelRefund" class="secondary">
            Cancel
          </button>
        </div>
      </form>

      <!-- 退款须知 -->
      <div class="refund-notice">
        <h4>Refund Policy:</h4>
        <ul>
          <li>Refund amount cannot exceed the paid amount</li>
          <li>Refund processing time: 1-3 business days</li>
          <li>Partial refunds are supported</li>
          <li>Once submitted, refund requests cannot be cancelled</li>
        </ul>
      </div>
    </div>

    <div v-else-if="stepNumber === '5'">
      <h2>Refund Status</h2>

      <!-- 退款信息卡片 -->
      <div v-if="orderStore.refundInfo" class="refund-info-card">
        <div class="info-section">
          <h3>Refund Information</h3>
          <div class="info-grid">
            <div class="info-item">
              <span class="label">Refund No:</span>
              <span class="value">{{ orderStore.refundInfo.refund_no }}</span>
            </div>
            <div class="info-item">
              <span class="label">Order No:</span>
              <span class="value">{{ orderStore.refundInfo.order_no }}</span>
            </div>
            <div class="info-item">
              <span class="label">Refund Amount:</span>
              <span class="value amount">¥{{ orderStore.refundInfo.refund_amount }}</span>
            </div>
            <div class="info-item">
              <span class="label">Status:</span>
              <span :class="['status-badge', getRefundStatusClass(orderStore.refundInfo.status)]">
                {{ formatRefundStatus(orderStore.refundInfo.status) }}
              </span>
            </div>
            <div class="info-item" v-if="orderStore.refundInfo.refund_reason">
              <span class="label">Reason:</span>
              <span class="value">{{ orderStore.refundInfo.refund_reason }}</span>
            </div>
            <div class="info-item" v-if="orderStore.refundInfo.channel_refund_no">
              <span class="label">Channel Refund No:</span>
              <span class="value">{{ orderStore.refundInfo.channel_refund_no }}</span>
            </div>
            <div class="info-item" v-if="orderStore.refundInfo.created_at">
              <span class="label">Applied At:</span>
              <span class="value">{{ formatDate(orderStore.refundInfo.created_at) }}</span>
            </div>
            <div class="info-item" v-if="orderStore.refundInfo.refunded_at">
              <span class="label">Refunded At:</span>
              <span class="value">{{ formatDate(orderStore.refundInfo.refunded_at) }}</span>
            </div>
          </div>
        </div>

        <!-- 渠道响应信息 -->
        <div v-if="orderStore.refundInfo.channel_response" class="info-section">
          <h3>Channel Response</h3>
          <pre class="json-display">{{ JSON.stringify(orderStore.refundInfo.channel_response, null, 2) }}</pre>
        </div>
      </div>

      <!-- 查询表单（直接进入时显示） -->
      <div v-else class="refund-query-form">
        <div class="form-group">
          <label for="refundOrderNo">Order No / Refund No *</label>
          <input
            id="refundOrderNo"
            v-model="queryForm.orderNo"
            type="text"
            required
            placeholder="Enter order no or refund no"
          />
        </div>

        <div v-if="queryError" class="error-message">{{ queryError }}</div>

        <div class="button-group">
          <button @click="handleQueryRefund" :disabled="queryLoading || !queryForm.orderNo">
            {{ queryLoading ? 'Querying...' : 'Query Refund Status' }}
          </button>
          <button @click="handleBackToOrders" class="secondary">
            Back
          </button>
        </div>
      </div>

      <!-- 操作按钮 -->
      <div v-if="orderStore.refundInfo" class="button-group">
        <button @click="handleRefreshRefund" :disabled="queryLoading">
          {{ queryLoading ? 'Refreshing...' : 'Refresh Status' }}
        </button>
        <button @click="handleBackToOrders" class="secondary">
          Back to Orders
        </button>
      </div>
    </div>

    <div v-else-if="stepNumber === '6'">
      <h2>Order Details</h2>

      <!-- 订单信息卡片 -->
      <div v-if="orderStore.currentOrder" class="order-detail-card">
        <div class="info-section">
          <h3>Order Information</h3>
          <div class="info-grid">
            <div class="info-item">
              <span class="label">Order No:</span>
              <span class="value">{{ orderStore.currentOrder.order_no }}</span>
            </div>
            <div class="info-item">
              <span class="label">Product Name:</span>
              <span class="value">{{ orderStore.currentOrder.title || orderStore.currentOrder.product_name || 'N/A' }}</span>
            </div>
            <div class="info-item">
              <span class="label">Amount:</span>
              <span class="value amount">¥{{ orderStore.currentOrder.amount }}</span>
            </div>
            <div class="info-item">
              <span class="label">Status:</span>
              <span :class="['status-badge', getStatusClass(orderStore.currentOrder.status)]">
                {{ formatStatus(orderStore.currentOrder.status) }}
              </span>
            </div>
            <div class="info-item" v-if="orderStore.currentOrder.description">
              <span class="label">Description:</span>
              <span class="value">{{ orderStore.currentOrder.description }}</span>
            </div>
            <div class="info-item">
              <span class="label">Created At:</span>
              <span class="value">{{ formatDate(orderStore.currentOrder.created_at) }}</span>
            </div>
          </div>
        </div>

        <!-- 支付信息 -->
        <div v-if="orderStore.currentOrder.trade_no || orderStore.currentOrder.payment_no" class="info-section">
          <h3>Payment Information</h3>
          <div class="info-grid">
            <div class="info-item" v-if="orderStore.currentOrder.trade_no">
              <span class="label">Trade No:</span>
              <span class="value">{{ orderStore.currentOrder.trade_no }}</span>
            </div>
            <div class="info-item" v-if="orderStore.currentOrder.payment_no">
              <span class="label">Payment No:</span>
              <span class="value">{{ orderStore.currentOrder.payment_no }}</span>
            </div>
            <div class="info-item" v-if="orderStore.currentOrder.paid_at">
              <span class="label">Paid At:</span>
              <span class="value">{{ formatDate(orderStore.currentOrder.paid_at) }}</span>
            </div>
          </div>
        </div>

        <!-- 二维码显示区域（仅待支付订单显示） -->
        <div v-if="orderStore.currentOrder.status === 'PENDING' || orderStore.currentOrder.status === 'PAYING'" class="info-section">
          <h3>Payment QR Code</h3>
          <div v-if="orderStore.currentOrder.status === 'PAYING' && !detailQrCodeUrl && !detailQrError" class="info-message">
            <p>ℹ️ 订单正在支付中，请使用之前扫描的二维码完成支付</p>
          </div>
          <div v-else class="qr-container">
            <div v-if="detailQrLoading" class="qr-loading">Generating QR code...</div>
            <div v-else-if="detailQrError" class="error-message">{{ detailQrError }}</div>
            <div v-else-if="detailQrCodeUrl" class="qr-wrapper">
              <canvas ref="detailQrCanvas"></canvas>
              <p class="qr-instruction">
                Use Alipay Sandbox App to scan the QR code
              </p>
            </div>
            <div v-else class="qr-placeholder">
              <button @click="handleGenerateQRForDetail" :disabled="detailQrGenerating" class="primary">
                {{ detailQrGenerating ? 'Generating...' : 'Generate QR Code' }}
              </button>
            </div>
          </div>
        </div>

        <!-- 渠道响应信息 -->
        <div v-if="orderStore.currentOrder.channel_response" class="info-section">
          <h3>Channel Response</h3>
          <pre class="json-display">{{ JSON.stringify(orderStore.currentOrder.channel_response, null, 2) }}</pre>
        </div>

        <!-- 操作按钮 -->
        <div class="button-group">
          <button
            v-if="orderStore.currentOrder.status === 'PENDING' || orderStore.currentOrder.status === 'PAYING'"
            @click="handleRefreshOrderDetail"
            :disabled="detailRefreshing"
          >
            {{ detailRefreshing ? 'Refreshing...' : 'Refresh Status' }}
          </button>
          <button
            v-if="orderStore.currentOrder.status === 'PAID'"
            @click="handleGoToRefundFromDetail"
            class="warning"
          >
            Apply for Refund
          </button>
          <button @click="handleBackToOrdersFromDetail" class="secondary">
            Back to Orders
          </button>
        </div>
      </div>

      <div v-else class="error-message">
        <p>No order information found</p>
        <button @click="handleBackToOrdersFromDetail" class="primary">Back to Orders</button>
      </div>
    </div>

    <div v-else>
      <p>Other steps will be implemented in subsequent tasks</p>
    </div>
  </div>
</template>

<script setup>
import { ref, onMounted, onUnmounted, nextTick, watch } from 'vue'
import { useRouter } from 'vue-router'
import StepIndicator from '../components/StepIndicator.vue'
import { useOrderStore } from '../stores/order'
import { useUserStore } from '../stores/user'
import { paymentApi } from '../api/payment'
import QRCode from 'qrcode'

const props = defineProps({
  stepNumber: {
    type: String,
    required: true
  }
})

const router = useRouter()
const orderStore = useOrderStore()
const userStore = useUserStore()

const form = ref({
  productName: '',
  amount: '',
  description: ''
})

const loading = ref(false)
const error = ref('')
const qrCodeUrl = ref('')
const qrCanvas = ref(null)
const pollingTimer = ref(null)
const paymentTime = ref('')
const isPolling = ref(false)
const pendingRequests = ref(0)
const consecutiveErrors = ref(0)

// 订单列表相关
const listLoading = ref(false)
const listError = ref('')

// 退款相关
const refundForm = ref({
  amount: 0,
  reason: ''
})
const refundSuccess = ref('')
const refundLoading = ref(false)

// 退款查询相关
const queryForm = ref({
  orderNo: ''
})
const queryLoading = ref(false)
const queryError = ref('')

// Step 6 订单详情相关
const detailQrCodeUrl = ref('')
const detailQrCanvas = ref(null)
const detailQrLoading = ref(false)
const detailQrError = ref('')
const detailQrGenerating = ref(false)
const detailRefreshing = ref(false)
const isDetailPolling = ref(false)
const detailPollingTimer = ref(null)

async function handleCreateOrder() {
  // 从环境变量或sessionStorage获取userId和apiKey
  if (!userStore.userId || !userStore.apiKey) {
    error.value = '请先配置.env.local文件中的VITE_DEFAULT_USER_ID和VITE_DEFAULT_API_KEY'
    loading.value = false
    return
  }

  loading.value = true
  error.value = ''

  try {
    // 创建订单
    const response = await paymentApi.createOrder({
      order_no: "ORDER-" + Date.now(),
      amount: String(form.value.amount),
      description: form.value.description,
      channel: "alipay",
      user_id: userStore.userId
    })

    const orderData = response.data || response
    const alipayData = orderData.alipay_response || {}

    // 保存订单信息
    orderStore.setOrder({
      orderId: orderData.order_no || alipayData.out_trade_no,
      productName: form.value.productName,
      amount: form.value.amount,
      description: form.value.description,
      status: orderData.status || 'PENDING',
      tradeNo: alipayData.trade_no || '',
      paymentNo: orderData.payment_no || '',
      userId: userStore.userId  // 保存userId
    })

    // 如果响应中包含二维码，直接使用
    if (orderData.qr_code || alipayData.qr_code) {
      qrCodeUrl.value = orderData.qr_code || alipayData.qr_code
      // 保存二维码URL到localStorage
      orderStore.updateOrder({ qrCodeUrl: qrCodeUrl.value })
    }

    // 自动跳转到 Step 2
    router.push('/step/2')

  } catch (err) {
    error.value = err.response?.data?.message || err.message || 'Failed to create order'
  } finally {
    loading.value = false
  }
}

async function generateQRCode() {
  if (!userStore.userId) {
    error.value = '请先登录'
    throw new Error('User not logged in')
  }

  try {
    const response = await paymentApi.createQRPayment({
      order_no: orderStore.orderId,
      amount: orderStore.amount,
      channel: 'alipay',
      user_id: userStore.userId,
      subject: orderStore.productName || 'Payment'
    })

    if (response.data && response.data.qr_code) {
      qrCodeUrl.value = response.data.qr_code
      // 保存二维码URL到localStorage
      orderStore.updateOrder({ qrCodeUrl: qrCodeUrl.value })
    } else {
      throw new Error('No QR code in response')
    }
  } catch (err) {
    error.value = err.message || 'Failed to generate QR code'
    throw err
  }
}

function handleReset() {
  form.value = {
    productName: '',
    amount: '',
    description: ''
  }
  error.value = ''
}

function handleLogout() {
  userStore.logout()
  form.value = {
    productName: '',
    amount: '',
    description: ''
  }
  router.push('/step/1')
}

async function handleRefresh() {
  loading.value = true
  error.value = ''

  try {
    const response = await paymentApi.queryOrder(orderStore.orderId)

    // Update order store with latest status
    orderStore.updateOrder({
      status: response.data.status,
      tradeNo: response.data.alipay_response?.trade_no || orderStore.tradeNo
    })

    // If order is still pending or paying, ensure QR code is displayed
    if (response.data.status === 'PENDING' || response.data.status === 'PAYING') {
      // Re-render QR code if URL exists
      if (qrCodeUrl.value) {
        await nextTick()
        await renderQRCodeImpl()
      }
      // Otherwise generate new QR code
      else {
        await generateQRCode()
      }
    }
  } catch (err) {
    error.value = err.message || 'Failed to refresh order status'
  } finally {
    loading.value = false
  }
}

function handleBack() {
  router.push('/step/1')
}

async function handleGenerateQR() {
  const userId = orderStore.userId || userStore.userId
  if (!userId) {
    error.value = '缺少用户ID，请重新创建订单'
    loading.value = false
    return
  }

  loading.value = true
  error.value = ''

  try {
    const response = await paymentApi.createQRPayment({
      order_no: orderStore.orderId,
      amount: orderStore.amount,
      channel: 'alipay',
      user_id: userId,
      subject: orderStore.productName || 'Payment'
    })

    if (response.data && response.data.qr_code) {
      qrCodeUrl.value = response.data.qr_code
      router.push('/step/3')
    } else {
      error.value = 'Failed to generate QR code: no QR code in response'
    }
  } catch (err) {
    error.value = err.message || 'Failed to generate QR code'
  } finally {
    loading.value = false
  }
}

async function handleCheckPayment() {
  loading.value = true
  error.value = ''

  try {
    const response = await paymentApi.queryOrder(orderStore.orderId)

    // Update order store with latest status
    orderStore.updateOrder({
      status: response.data.status,
      tradeNo: response.data.alipay_response?.trade_no || orderStore.tradeNo
    })

    // If payment is completed, go to step 4
    if (response.data.status === 'PAID') {
      router.push('/step/4')
    } else if (response.data.status === 'FAILED') {
      error.value = 'Payment failed. Please try again.'
    } else {
      error.value = 'Payment not completed yet. Current status: ' + response.data.status
    }
  } catch (err) {
    error.value = err.message || 'Failed to check payment status'
  } finally {
    loading.value = false
  }
}

function handleBackToStep2() {
  router.push('/step/2')
}

function getStatusClass(status) {
  switch (status) {
    case 'PAID':
      return 'status-paid'
    case 'PAYING':
      return 'status-paying'
    case 'FAILED':
      return 'status-failed'
    case 'REFUNDED':
      return 'status-refunded'
    default:
      return 'status-pending'
  }
}

function formatStatus(status) {
  const statusMap = {
    'PENDING': 'Pending',
    'PAID': 'Paid',
    'PAYING': 'Paying',
    'FAILED': 'Failed',
    'REFUNDED': 'Refunded'
  }
  return statusMap[status] || status
}

function formatDate(dateString) {
  if (!dateString) return 'N/A'
  const date = new Date(dateString)
  return date.toLocaleString('zh-CN', {
    year: 'numeric',
    month: '2-digit',
    day: '2-digit',
    hour: '2-digit',
    minute: '2-digit',
    second: '2-digit',
    hour12: false
  }).replace(/\//g, '-')
}

// 订单列表相关函数
async function loadOrders() {
  listLoading.value = true
  listError.value = ''

  try {
    const params = {}
    // 对于"Pending Payment"筛选器，需要同时获取PENDING和PAYING状态的订单
    // 所以不发送status参数，在前端进行筛选
    if (orderStore.orderFilter !== 'all' && orderStore.orderFilter !== 'PENDING') {
      params.status = orderStore.orderFilter
    }
    console.log('[loadOrders] Fetching orders with params:', params)
    const response = await paymentApi.queryOrderList(params)
    console.log('[loadOrders] Response received:', response)
    console.log('[loadOrders] Response data:', response.data)

    // 检查响应格式
    let orders = []
    if (Array.isArray(response.data)) {
      orders = response.data
    } else if (response.data && Array.isArray(response.data.data)) {
      orders = response.data.data
    }

    console.log('[loadOrders] Final orders array:', orders)

    // 如果筛选器是"Pending Payment"，在前端筛选PENDING和PAYING状态
    if (orderStore.orderFilter === 'PENDING') {
      orders = orders.filter(order => order.status === 'PENDING' || order.status === 'PAYING')
      console.log('[loadOrders] Filtered for PENDING/PAYING:', orders)
    }

    orderStore.setOrders(orders)

    // 不再自动同步PAYING状态的订单，避免不必要的后端和API压力
    // 用户可以在订单详情页面手动刷新状态
  } catch (err) {
    console.error('[loadOrders] Error loading orders:', err)
    listError.value = err.message || 'Failed to load orders'
  } finally {
    listLoading.value = false
  }
}

// 自动同步PAYING状态的订单
async function syncPayingOrders(orders) {
  const payingOrders = orders.filter(order => order.status === 'PAYING')

  if (payingOrders.length === 0) {
    console.log('[SYNC] No PAYING orders to sync')
    return
  }

  console.log(`[SYNC] Found ${payingOrders.length} PAYING orders, syncing...`)

  // 并发同步所有PAYING订单（限制并发数为5）
  const batchSize = 5
  for (let i = 0; i < payingOrders.length; i += batchSize) {
    const batch = payingOrders.slice(i, i + batchSize)
    await Promise.all(batch.map(async (order) => {
      try {
        console.log(`[SYNC] Syncing order: ${order.order_no}`)
        const response = await paymentApi.queryOrder(order.order_no)
        const updatedOrder = response.data

        // 更新订单列表中的状态
        const index = orderStore.orders.findIndex(o => o.order_no === order.order_no)
        if (index !== -1) {
          orderStore.orders[index] = {
            ...orderStore.orders[index],
            status: updatedOrder.status,
            trade_no: updatedOrder.alipay_response?.trade_no || order.trade_no
          }
          console.log(`[SYNC] Order ${order.order_no} status updated to: ${updatedOrder.status}`)
        }
      } catch (err) {
        console.error(`[SYNC] Failed to sync order ${order.order_no}:`, err.message)
      }
    }))
  }

  console.log('[SYNC] Sync completed')
}

function handleFilterChange() {
  loadOrders()
}

function handleRefreshOrders() {
  loadOrders()
}

function handleViewOrderDetail() {
  orderStore.setCurrentOrder({
    order_no: orderStore.orderId,
    product_name: orderStore.productName,
    amount: orderStore.amount,
    description: orderStore.description,
    status: orderStore.status,
    trade_no: orderStore.tradeNo,
    payment_no: orderStore.paymentNo,
    created_at: orderStore.createdAt
  })
  router.push('/step/6')
}

function handleGoToRefund() {
  orderStore.setCurrentOrder({
    order_no: orderStore.orderId,
    product_name: orderStore.productName,
    amount: orderStore.amount,
    trade_no: orderStore.tradeNo
  })
  router.push('/step/4')
}

function handleViewOrderDetailFromList(order) {
  orderStore.setCurrentOrder(order)
  router.push('/step/6')
}

function handleGoToRefundFromList(order) {
  orderStore.setCurrentOrder(order)
  router.push('/step/4')
}

// 退款相关函数
async function handleSubmitRefund() {
  refundLoading.value = true
  listError.value = ''
  refundSuccess.value = ''

  try {
    const response = await paymentApi.refund({
      order_no: orderStore.currentOrder.order_no,
      amount: String(refundForm.value.amount),
      reason: refundForm.value.reason
    })

    // 保存退款信息
    orderStore.setRefundInfo(response.data)

    refundSuccess.value = 'Refund request submitted successfully!'

    // 延迟跳转到 Step 5
    setTimeout(() => {
      router.push('/step/5')
    }, 1500)

  } catch (err) {
    listError.value = err.message || 'Failed to submit refund request'
  } finally {
    refundLoading.value = false
  }
}

function handleCancelRefund() {
  router.push('/step/3')
}

// 退款查询相关函数
async function handleQueryRefund() {
  queryLoading.value = true
  queryError.value = ''

  try {
    // 优先使用 refundInfo 中的 refund_no，如果没有则使用表单输入的值
    const refundNo = orderStore.refundInfo?.refund_no || queryForm.value.orderNo
    if (!refundNo) {
      queryError.value = 'Please enter refund no'
      return
    }
    const response = await paymentApi.queryRefund(refundNo)
    orderStore.setRefundInfo(response.data)
  } catch (err) {
    queryError.value = err.message || 'Failed to query refund status'
  } finally {
    queryLoading.value = false
  }
}

async function handleRefreshRefund() {
  if (orderStore.refundInfo?.refund_no) {
    await handleQueryRefund()
  }
}

function handleBackToOrders() {
  router.push('/step/3')
}

function formatRefundStatus(status) {
  const statusMap = {
    'PENDING': 'Processing',
    'SUCCESS': 'Refunded',
    'FAILED': 'Failed',
    'UNKNOWN': 'Unknown'
  }
  return statusMap[status] || status
}

function getRefundStatusClass(status) {
  switch (status) {
    case 'SUCCESS': return 'status-refunded'
    case 'FAILED': return 'status-failed'
    case 'PENDING': return 'status-paying'
    default: return 'status-pending'
  }
}

// 订单详情相关函数
function handleGoToRefundFromDetail() {
  if (orderStore.currentOrder) {
    router.push('/step/4')
  }
}

function handleBackToOrdersFromDetail() {
  router.push('/step/3')
}

// Step 6 生成二维码
async function handleGenerateQRForDetail() {
  if (!orderStore.currentOrder?.order_no || !userStore.userId) {
    detailQrError.value = '缺少必要信息，无法生成二维码'
    return
  }

  detailQrGenerating.value = true
  detailQrError.value = ''

  try {
    const response = await paymentApi.createQRPayment({
      order_no: orderStore.currentOrder.order_no,
      amount: orderStore.currentOrder.amount,
      channel: 'alipay',
      user_id: userStore.userId,
      subject: orderStore.currentOrder.title || orderStore.currentOrder.product_name || 'Payment'
    })

    if (response.data && response.data.qr_code) {
      detailQrCodeUrl.value = response.data.qr_code
      // 保存二维码URL到localStorage
      orderStore.updateOrder({ qrCodeUrl: detailQrCodeUrl.value })
      await nextTick()
      await renderDetailQRCode()
    } else {
      detailQrError.value = 'Failed to generate QR code: no QR code in response'
    }
  } catch (err) {
    detailQrError.value = err.message || 'Failed to generate QR code'
  } finally {
    detailQrGenerating.value = false
  }
}

// Step 6 刷新订单详情
async function handleRefreshOrderDetail() {
  if (!orderStore.currentOrder?.order_no) {
    return
  }

  detailRefreshing.value = true
  detailQrError.value = ''

  try {
    const response = await paymentApi.queryOrder(orderStore.currentOrder.order_no)
    orderStore.setCurrentOrder(response.data)

    // 如果支付完成，清除二维码显示
    if (response.data.status === 'PAID') {
      detailQrCodeUrl.value = ''
    }
  } catch (err) {
    detailQrError.value = err.message || 'Failed to refresh order status'
  } finally {
    detailRefreshing.value = false
  }
}

// Step 6 渲染二维码
async function renderDetailQRCode() {
  if (!detailQrCanvas.value || !detailQrCodeUrl.value) {
    return
  }

  try {
    await QRCode.toCanvas(detailQrCanvas.value, detailQrCodeUrl.value, {
      width: 256,
      margin: 2,
      color: {
        dark: '#000000',
        light: '#FFFFFF'
      }
    })
  } catch (err) {
    console.error('Failed to render detail QR code:', err)
    detailQrError.value = 'Failed to render QR code: ' + err.message
  }
}

// Step 6 开始轮询
function startDetailPolling() {
  if (isDetailPolling.value) {
    console.log('[DETAIL POLLING] Already polling, skipping duplicate request')
    return
  }

  stopDetailPolling()
  isDetailPolling.value = true

  detailPollingTimer.value = setInterval(async () => {
    if (!orderStore.currentOrder?.order_no) {
      return
    }

    try {
      console.log('[DETAIL POLLING] Querying order status for:', orderStore.currentOrder.order_no)
      const response = await paymentApi.queryOrder(orderStore.currentOrder.order_no)

      if (!response?.data) {
        console.error('[DETAIL POLLING] Invalid response')
        return
      }

      orderStore.setCurrentOrder(response.data)

      if (response.data.status === 'PAID') {
        stopDetailPolling()
        detailQrCodeUrl.value = ''
        console.log('[DETAIL POLLING] Payment completed!')
      } else if (response.data.status === 'FAILED') {
        stopDetailPolling()
        detailQrError.value = 'Payment failed. Please try again.'
        console.log('[DETAIL POLLING] Payment failed')
      } else {
        console.log('[DETAIL POLLING] Payment not completed yet, status:', response.data.status)
      }
    } catch (err) {
      console.error('[DETAIL POLLING] Polling error:', err)
    }
  }, 5000)

  console.log('[DETAIL POLLING] Started polling for order:', orderStore.currentOrder.order_no)
}

// Step 6 停止轮询
function stopDetailPolling() {
  if (detailPollingTimer.value) {
    clearInterval(detailPollingTimer.value)
    detailPollingTimer.value = null
    console.log('[DETAIL POLLING] Polling stopped')
  }
  isDetailPolling.value = false
}

// Render QR code when component is mounted
onMounted(async () => {
  // 优先从环境变量加载（开发/测试环境）
  userStore.loadFromEnv()

  // 如果环境变量没有，再从sessionStorage加载
  if (!userStore.isLoggedIn) {
    userStore.loadFromSession()
  }

  // Step 2: Render QR code
  if (props.stepNumber === '2' && qrCodeUrl.value) {
    await nextTick()
    renderQRCodeImpl()
    startPolling()
  }

  // Step 3: Load order list
  if (props.stepNumber === '3') {
    loadOrders()
  }

  // Step 4: 初始化退款表单
  if (props.stepNumber === '4' && orderStore.currentOrder) {
    refundForm.value = {
      amount: orderStore.currentOrder.amount,
      reason: ''
    }
  }

  // Step 5: 如果有退款信息，自动查询最新状态
  if (props.stepNumber === '5' && orderStore.refundInfo?.refund_no) {
    await handleQueryRefund()
  }
})

// Watch for step changes to handle route navigation within the same component
watch(() => props.stepNumber, async (newStep, oldStep) => {
  console.log('[Watch] Step changed from', oldStep, 'to', newStep)

  // Step 6: 查询订单详情
  if (newStep === '6') {
    console.log('[Step 6] Entered Step 6 logic (from watch)')
    console.log('[Step 6] currentOrder:', orderStore.currentOrder)

    if (!orderStore.currentOrder?.order_no) {
      console.log('[Step 6] No order_no in currentOrder, skipping')
      return
    }

    console.log('[Step 6] Processing order:', orderStore.currentOrder.order_no)

    // 在调用API之前，先保存从订单列表传来的 channel_response（包含qr_code）
    const existingChannelResponse = orderStore.currentOrder?.channel_response
    console.log('[Step 6] Saved existing channel_response from order list:', existingChannelResponse)

    queryLoading.value = true
    queryError.value = ''
    try {
      const response = await paymentApi.queryOrder(orderStore.currentOrder.order_no)
      console.log('[Step 6] API response received:', response)
      console.log('[Step 6] Full response.data:', response.data)

      // 合并订单列表数据和API响应数据，优先保留订单列表中的 channel_response（包含qr_code）
      const mergedOrder = {
        ...response.data,
        channel_response: existingChannelResponse || response.data.channel_response
      }

      console.log('[Step 6] Merged order data:', mergedOrder)
      orderStore.setCurrentOrder(mergedOrder)

      // 先从localStorage读取保存的二维码URL
      const savedQrCode = orderStore.getQrCodeUrl(orderStore.currentOrder.order_no)

      // 从多个可能的字段中读取二维码URL
      let backendQrCode = null

      // 优先从 orderStore.currentOrder.channel_response 中读取（创建订单时保存的数据）
      if (orderStore.currentOrder.channel_response) {
        const channelResp = orderStore.currentOrder.channel_response
        console.log('[Step 6] Found channel_response in currentOrder, type:', typeof channelResp)
        console.log('[Step 6] channel_response content:', channelResp)
        if (typeof channelResp === 'string') {
          try {
            const parsed = JSON.parse(channelResp)
            console.log('[Step 6] Parsed channel_response:', parsed)
            if (parsed.qr_code) {
              backendQrCode = parsed.qr_code
              console.log('[Step 6] Found qr_code in currentOrder.channel_response (string JSON):', backendQrCode)
            }
          } catch (e) {
            console.warn('[Step 6] Failed to parse channel_response:', e)
          }
        } else if (typeof channelResp === 'object') {
          console.log('[Step 6] channel_response is object, keys:', Object.keys(channelResp))
          if (channelResp.qr_code) {
            backendQrCode = channelResp.qr_code
            console.log('[Step 6] Found qr_code in currentOrder.channel_response (object):', backendQrCode)
          } else {
            console.warn('[Step 6] channel_response object does not have qr_code property')
          }
        }
      } else {
        console.log('[Step 6] No channel_response found in currentOrder')
      }

      // 如果 currentOrder 中没有，再从 API 响应中读取
      if (!backendQrCode && response.data.qr_code) {
        backendQrCode = response.data.qr_code
        console.log('[Step 6] Found qr_code in response.data.qr_code')
      }
      if (!backendQrCode && response.data.alipay_response?.qr_code) {
        backendQrCode = response.data.alipay_response.qr_code
        console.log('[Step 6] Found qr_code in response.data.alipay_response')
      }

      const finalQrCode = savedQrCode || backendQrCode

      console.log('[Step 6] QR code extraction result:', {
        saved: savedQrCode,
        backend: backendQrCode,
        final: finalQrCode,
        hasChannelResponse: !!response.data.channel_response,
        channelResponseType: typeof response.data.channel_response
      })

      // 根据订单状态处理二维码
      if (response.data.status === 'PENDING') {
        // PENDING状态：如果有保存的二维码URL，直接使用；否则生成新的
        if (finalQrCode) {
          console.log('[Step 6] PENDING status: Using saved QR code')
          detailQrCodeUrl.value = finalQrCode
          await nextTick()
          await renderDetailQRCode()
        } else {
          console.log('[Step 6] PENDING status: No QR code found, generating new one')
          await handleGenerateQRForDetail()
        }
      } else if (response.data.status === 'PAYING') {
        // PAYING状态：优先使用已有的二维码URL
        console.log('[Step 6] PAYING status: finalQrCode =', finalQrCode)
        if (finalQrCode) {
          console.log('[Step 6] PAYING status: Using existing QR code from order list')
          detailQrCodeUrl.value = finalQrCode
          detailQrError.value = ''
          await nextTick()
          await renderDetailQRCode()
        } else {
          // PAYING状态但没有找到二维码：显示提示信息
          console.log('[Step 6] PAYING status: No QR code found, showing error message')
          detailQrError.value = '订单正在支付中，请使用之前扫描的二维码完成支付'
        }
      }
    } catch (err) {
      console.error('[Step 6] Error:', err)
      queryError.value = err.message || 'Failed to query order details'
    } finally {
      queryLoading.value = false
    }
  }
})

// Start polling order status
function startPolling() {
  // Prevent starting if already polling
  if (isPolling.value) {
    console.log('[POLLING] Already polling, skipping duplicate request')
    return
  }

  // Clear existing timer if any
  stopPolling()

  // Set polling flag
  isPolling.value = true
  consecutiveErrors.value = 0

  pollingTimer.value = setInterval(async () => {
    // Prevent overlapping requests
    if (pendingRequests.value > 0) {
      console.log('[POLLING] Previous request still pending, skipping this poll')
      return
    }

    try {
      pendingRequests.value++
      console.log('[POLLING] Querying order status for:', orderStore.orderId)
      const response = await paymentApi.queryOrder(orderStore.orderId)
      console.log('[POLLING] Response received:', response)

      // Check if response exists
      if (!response) {
        console.error('[POLLING] Response is undefined or null')
        return
      }

      // Check if response.data exists
      if (!response.data) {
        console.error('[POLLING] Response.data is undefined:', response)
        return
      }

      // Reset error counter on successful request
      consecutiveErrors.value = 0

      // Update order store with latest status
      orderStore.updateOrder({
        status: response.data.status,
        tradeNo: response.data.alipay_response?.trade_no || orderStore.tradeNo
      })

      // Check if payment is completed
      if (response.data.status === 'PAID') {
        stopPolling()
        // Set payment time
        paymentTime.value = new Date().toLocaleString('zh-CN', {
          year: 'numeric',
          month: '2-digit',
          day: '2-digit',
          hour: '2-digit',
          minute: '2-digit',
          second: '2-digit',
          hour12: false
        }).replace(/\//g, '-')
        console.log('[POLLING] Payment completed!')
        return  // 立即退出，避免重复执行
      } else if (response.data.status === 'FAILED') {
        stopPolling()
        error.value = 'Payment failed. Please try again.'
        console.log('[POLLING] Payment failed')
        return  // 立即退出
      } else {
        console.log('[POLLING] Payment not completed yet, status:', response.data.status)
      }
    } catch (err) {
      consecutiveErrors.value++
      console.error('[POLLING] Polling error:', err)
      console.error('[POLLING] Error message:', err.message)
      console.error('[POLLING] Consecutive errors:', consecutiveErrors.value)

      // Stop polling after 3 consecutive errors
      if (consecutiveErrors.value >= 3) {
        console.error('[POLLING] Too many consecutive errors, stopping polling')
        stopPolling()
        error.value = 'Unable to check payment status. Please manually refresh the order list.'
      }
    } finally {
      pendingRequests.value--
    }
  }, 5000) // Poll every 5 seconds (increased from 3 to reduce server load)

  console.log('Polling started for order:', orderStore.orderId)
}

// Stop polling order status
function stopPolling() {
  if (pollingTimer.value) {
    clearInterval(pollingTimer.value)
    pollingTimer.value = null
    console.log('Polling stopped')
  }
  // Reset polling flag
  isPolling.value = false
  // Wait a bit for pending requests to complete
  setTimeout(() => {
    if (pendingRequests.value === 0) {
      consecutiveErrors.value = 0
    }
  }, 1000)
}

// Actual QR code rendering implementation
async function renderQRCodeImpl() {
  if (!qrCanvas.value || !qrCodeUrl.value) {
    return
  }

  try {
    await QRCode.toCanvas(qrCanvas.value, qrCodeUrl.value, {
      width: 256,
      margin: 2,
      color: {
        dark: '#000000',
        light: '#FFFFFF'
      }
    })
  } catch (err) {
    console.error('Failed to render QR code:', err)
    error.value = 'Failed to render QR code: ' + err.message
  }
}

// Watch for qrCodeUrl changes
async function renderQRCode() {
  if (qrCodeUrl.value) {
    // Wait for DOM to update
    await nextTick()

    // Try multiple times to find canvas
    let attempts = 0
    const maxAttempts = 5

    const tryRender = async () => {
      attempts++

      if (qrCanvas.value) {
        await renderQRCodeImpl()
      } else if (attempts < maxAttempts) {
        // Wait and try again
        setTimeout(() => tryRender(), 100)
      } else {
        error.value = 'Failed to render QR code: Canvas not found'
      }
    }

    await tryRender()
  }
}

// Watch for step changes and qrCodeUrl changes
watch(() => props.stepNumber, async (newStep) => {
  // Stop polling when leaving step 2
  if (newStep !== '2') {
    stopPolling()
  }

  // Render QR code when entering step 2
  if (newStep === '2' && qrCodeUrl.value) {
    await renderQRCode()
    // Start polling for payment status
    startPolling()
  }

  // Initialize refund form when entering step 4
  if (newStep === '4' && orderStore.currentOrder) {
    refundForm.value = {
      amount: orderStore.currentOrder.amount,
      reason: ''
    }
  }
})

// Call renderQRCode when qrCodeUrl changes
const unwatchQRCode = ref()
unwatchQRCode.value = watch(qrCodeUrl, async () => {
  await renderQRCode()
  // If we're in step 2 and QR code URL is set, start polling
  if (props.stepNumber === '2' && qrCodeUrl.value) {
    startPolling()
  }
})

// Cleanup watcher on unmount
onUnmounted(() => {
  if (unwatchQRCode.value) {
    unwatchQRCode.value()
  }
  // Stop polling when component unmounts
  stopPolling()
})
</script>

<style scoped>
.step-view {
  max-width: 800px;
  margin: 0 auto;
}

.user-info-bar {
  display: flex;
  justify-content: space-between;
  align-items: center;
  background-color: #f0f8ff;
  padding: 12px 20px;
  border-radius: 8px;
  margin-bottom: 20px;
  border: 1px solid #b0d4f1;
}

.user-info {
  font-size: 14px;
  color: #333;
}

.logout-btn {
  background-color: #dc3545;
  color: white;
  border: none;
  padding: 6px 16px;
  border-radius: 4px;
  cursor: pointer;
  font-size: 14px;
  transition: background-color 0.2s;
}

.logout-btn:hover {
  background-color: #c82333;
}

.login-notice {
  background-color: #fff3cd;
  border: 1px solid #ffc107;
  color: #856404;
  padding: 12px;
  border-radius: 8px;
  margin-bottom: 20px;
  text-align: center;
}

h2 {
  margin-bottom: 24px;
  font-size: 24px;
}

.order-form {
  background-color: #ffffff;
  padding: 24px;
  border: 1px solid #e0e0e0;
  border-radius: 8px;
}

.form-group {
  margin-bottom: 20px;
}

.form-group label {
  display: block;
  margin-bottom: 8px;
  font-weight: bold;
  color: #000000;
}

.form-group input,
.form-group textarea {
  width: 100%;
  padding: 10px;
  border: 1px solid #e0e0e0;
  border-radius: 4px;
  font-size: 14px;
  font-family: inherit;
}

.form-group input:focus,
.form-group textarea:focus {
  outline: none;
  border-color: #0066cc;
}

.error-message {
  color: #dc3545;
  padding: 12px;
  background-color: #f8d7da;
  border: 1px solid #f5c6cb;
  border-radius: 4px;
  margin-bottom: 20px;
}

.info-message {
  color: #004085;
  padding: 12px;
  background-color: #cce5ff;
  border: 1px solid #b8daff;
  border-radius: 4px;
  margin-bottom: 20px;
}

.info-message p {
  margin: 0;
}

.button-group {
  display: flex;
  gap: 12px;
}

button {
  padding: 10px 20px;
  border: none;
  border-radius: 4px;
  font-size: 14px;
  font-weight: bold;
  cursor: pointer;
  transition: all 0.2s;
}

button[type="submit"] {
  background-color: #0066cc;
  color: #ffffff;
}

button[type="submit"]:hover:not(:disabled) {
  background-color: #0052a3;
}

button[type="submit"]:disabled {
  background-color: #cccccc;
  cursor: not-allowed;
}

button.secondary {
  background-color: #ffffff;
  color: #000000;
  border: 1px solid #e0e0e0;
}

button.secondary:hover {
  background-color: #f5f5f5;
}

button:disabled:not([type="submit"]) {
  background-color: #cccccc;
  cursor: not-allowed;
}

.order-details {
  background-color: #ffffff;
  padding: 24px;
  border: 1px solid #e0e0e0;
  border-radius: 8px;
  margin-bottom: 20px;
}

.detail-group {
  display: flex;
  justify-content: space-between;
  align-items: center;
  padding: 12px 0;
  border-bottom: 1px solid #f0f0f0;
}

.detail-group:last-child {
  border-bottom: none;
}

.detail-group label {
  font-weight: bold;
  color: #666666;
  min-width: 150px;
}

.detail-value {
  color: #000000;
  text-align: right;
}

.detail-value.amount {
  font-size: 20px;
  font-weight: bold;
  color: #0066cc;
}

.status-badge {
  padding: 6px 12px;
  border-radius: 12px;
  font-size: 12px;
  font-weight: bold;
  text-transform: uppercase;
}

.status-pending {
  background-color: #fff3cd;
  color: #856404;
}

.status-paying {
  background-color: #cce5ff;
  color: #004085;
}

.status-paid {
  background-color: #d4edda;
  color: #155724;
}

.status-failed {
  background-color: #f8d7da;
  color: #721c24;
}

.status-refunded {
  background-color: #e2e3e5;
  color: #383d41;
}

.qr-container {
  background-color: #ffffff;
  padding: 40px;
  border: 1px solid #e0e0e0;
  border-radius: 8px;
  margin-bottom: 20px;
  text-align: center;
}

.qr-loading {
  padding: 40px;
  color: #666666;
}

.qr-wrapper {
  display: flex;
  flex-direction: column;
  align-items: center;
  gap: 20px;
}

.qr-wrapper canvas {
  border: 2px solid #e0e0e0;
  border-radius: 8px;
  padding: 10px;
  background-color: #ffffff;
}

.qr-instruction {
  color: #666666;
  font-size: 14px;
  margin: 0;
}

.qr-details {
  background-color: #f8f9fa;
  padding: 15px;
  border-radius: 4px;
  width: 100%;
  max-width: 400px;
}

.qr-details p {
  margin: 5px 0;
  color: #333333;
  text-align: left;
}

.qr-details strong {
  color: #0066cc;
  display: inline-block;
  min-width: 100px;
}

/* Step 3: 订单列表样式 */
.filter-bar {
  display: flex;
  justify-content: space-between;
  align-items: center;
  margin-bottom: 20px;
  padding: 15px;
  background-color: #f8f9fa;
  border-radius: 8px;
}

.filter-group {
  display: flex;
  align-items: center;
  gap: 10px;
}

.filter-group label {
  font-weight: 600;
  color: #333333;
}

.filter-group select {
  padding: 8px 12px;
  border: 1px solid #ddd;
  border-radius: 4px;
  background-color: white;
  font-size: 14px;
}

.refresh-btn {
  padding: 8px 16px;
  background-color: #6c757d;
  color: white;
  border: none;
  border-radius: 4px;
  cursor: pointer;
  font-size: 14px;
}

.refresh-btn:hover:not(:disabled) {
  background-color: #5a6268;
}

.refresh-btn:disabled {
  opacity: 0.6;
  cursor: not-allowed;
}

.loading-message,
.empty-message {
  text-align: center;
  padding: 40px 20px;
  color: #666666;
  font-size: 16px;
}

.order-list {
  display: grid;
  gap: 15px;
}

.order-card {
  background: white;
  border-radius: 8px;
  padding: 15px;
  box-shadow: 0 2px 4px rgba(0, 0, 0, 0.1);
  border-left: 4px solid #ddd;
  transition: box-shadow 0.2s;
}

.order-card:hover {
  box-shadow: 0 4px 8px rgba(0, 0, 0, 0.15);
}

.order-card.status-pending {
  border-left-color: #ffc107;
}

.order-card.status-paid {
  border-left-color: #28a745;
}

.order-card.status-paying {
  border-left-color: #007bff;
}

.order-card.status-failed {
  border-left-color: #dc3545;
}

.order-card.status-refunded {
  border-left-color: #6c757d;
}

.order-card-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  margin-bottom: 12px;
  padding-bottom: 12px;
  border-bottom: 1px solid #e5e7eb;
}

.order-no {
  font-weight: 600;
  color: #333333;
  font-size: 14px;
}

.status-badge {
  padding: 4px 12px;
  border-radius: 12px;
  font-size: 12px;
  font-weight: 600;
  text-transform: uppercase;
}

.status-badge.status-pending {
  background-color: #fff3cd;
  color: #856404;
}

.status-badge.status-paid {
  background-color: #d4edda;
  color: #155724;
}

.status-badge.status-paying {
  background-color: #cce5ff;
  color: #004085;
}

.status-badge.status-failed {
  background-color: #f8d7da;
  color: #721c24;
}

.status-badge.status-refunded {
  background-color: #e2e3e5;
  color: #383d41;
}

.order-card-body {
  display: flex;
  flex-direction: column;
  gap: 12px;
}

.order-info {
  display: flex;
  flex-direction: column;
  gap: 8px;
}

.info-row {
  display: flex;
  justify-content: space-between;
  align-items: center;
  font-size: 14px;
}

.info-row .label {
  color: #666666;
  font-weight: 500;
}

.info-row .value {
  color: #333333;
  font-weight: 600;
}

.info-row .value.amount {
  color: #28a745;
  font-size: 16px;
}

.order-actions {
  display: flex;
  gap: 10px;
  margin-top: 8px;
}

.action-btn {
  flex: 1;
  padding: 8px 16px;
  border: none;
  border-radius: 4px;
  font-size: 14px;
  font-weight: 600;
  cursor: pointer;
  transition: background-color 0.2s;
}

.action-btn.primary {
  background-color: #0066cc;
  color: white;
}

.action-btn.primary:hover {
  background-color: #0052a3;
}

.action-btn.warning {
  background-color: #ffc107;
  color: #333333;
}

.action-btn.warning:hover {
  background-color: #e0a800;
}

/* Step 4: 退款申请样式 */
.order-preview {
  background: white;
  border-radius: 8px;
  padding: 20px;
  margin-bottom: 20px;
  box-shadow: 0 2px 4px rgba(0, 0, 0, 0.1);
}

.order-preview h3 {
  margin: 0 0 15px 0;
  font-size: 16px;
  font-weight: 600;
  color: #333333;
}

.preview-row {
  display: flex;
  justify-content: space-between;
  padding: 10px 0;
  border-bottom: 1px solid #e5e7eb;
}

.preview-row:last-child {
  border-bottom: none;
}

.preview-row .label {
  font-weight: 600;
  color: #666666;
}

.preview-row .value {
  color: #333333;
}

.preview-row .value.amount {
  color: #28a745;
  font-weight: 700;
  font-size: 16px;
}

.refund-form {
  background: white;
  border-radius: 8px;
  padding: 20px;
  margin-bottom: 20px;
  box-shadow: 0 2px 4px rgba(0, 0, 0, 0.1);
}

.refund-notice {
  background: #fff3cd;
  border-left: 4px solid #ffc107;
  border-radius: 4px;
  padding: 15px 20px;
  margin-top: 20px;
}

.refund-notice h4 {
  margin: 0 0 10px 0;
  color: #856404;
  font-size: 16px;
}

.refund-notice ul {
  margin: 0;
  padding-left: 20px;
  color: #856404;
}

.refund-notice li {
  margin: 5px 0;
  font-size: 14px;
}

/* Step 5: 退款状态查询样式 */
.refund-info-card {
  background: white;
  border-radius: 8px;
  padding: 20px;
  margin-bottom: 20px;
  box-shadow: 0 2px 4px rgba(0, 0, 0, 0.1);
}

.refund-info-card .info-section {
  margin-bottom: 20px;
}

.refund-info-card .info-section:last-child {
  margin-bottom: 0;
}

.refund-info-card h3 {
  margin: 0 0 15px 0;
  font-size: 16px;
  font-weight: 600;
  color: #333333;
}

.info-grid {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(250px, 1fr));
  gap: 15px;
}

.info-item {
  display: flex;
  flex-direction: column;
  gap: 5px;
}

.info-item .label {
  font-size: 12px;
  color: #666666;
  font-weight: 600;
  text-transform: uppercase;
}

.info-item .value {
  font-size: 14px;
  color: #333333;
  font-weight: 500;
}

.info-item .value.amount {
  color: #28a745;
  font-weight: 700;
  font-size: 16px;
}

.json-display {
  background-color: #f8f9fa;
  border: 1px solid #e5e7eb;
  border-radius: 4px;
  padding: 15px;
  overflow-x: auto;
  font-size: 12px;
  color: #333333;
  margin: 0;
}

.refund-query-form {
  background: white;
  border-radius: 8px;
  padding: 20px;
  box-shadow: 0 2px 4px rgba(0, 0, 0, 0.1);
  max-width: 500px;
  margin: 0 auto;
}

/* Step 6: 订单详情样式 */
.order-detail-card {
  background: white;
  border-radius: 8px;
  padding: 0;
  box-shadow: 0 2px 4px rgba(0, 0, 0, 0.1);
  margin-bottom: 20px;
}

.order-detail-card .info-section {
  padding: 20px;
  border-bottom: 1px solid #e5e7eb;
}

.order-detail-card .info-section:last-child {
  border-bottom: none;
}

.order-detail-card h3 {
  margin: 0 0 15px 0;
  font-size: 16px;
  font-weight: 600;
  color: #1f2937;
}
</style>
