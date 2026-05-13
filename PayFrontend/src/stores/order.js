import { defineStore } from 'pinia'
import { ref } from 'vue'

export const useOrderStore = defineStore('order', () => {
  const orderId = ref('')
  const productName = ref('')
  const amount = ref(0)
  const description = ref('')
  const status = ref('')
  const createdAt = ref('')
  const tradeNo = ref('')
  const paymentNo = ref('')
  const userId = ref(null)  // 添加userId字段

  // 订单列表
  const orders = ref([])
  const orderFilter = ref('all')

  // 退款信息
  const refundInfo = ref(null)

  // 当前查看的订单（Step 6）
  const currentOrder = ref(null)

  function setOrder(order) {
    orderId.value = order.orderId || ''
    productName.value = order.productName || ''
    amount.value = order.amount || 0
    description.value = order.description || ''
    status.value = order.status || ''
    createdAt.value = order.createdAt || new Date().toISOString()
    tradeNo.value = order.tradeNo || ''
    paymentNo.value = order.paymentNo || ''
    userId.value = order.userId || null  // 保存userId

    // Persist to session storage
    sessionStorage.setItem('order', JSON.stringify({
      orderId: orderId.value,
      productName: productName.value,
      amount: amount.value,
      description: description.value,
      status: status.value,
      createdAt: createdAt.value,
      tradeNo: tradeNo.value,
      paymentNo: paymentNo.value,
      userId: userId.value  // 持久化userId
    }))
  }

  function updateOrder(updates) {
    if (updates.status) status.value = updates.status
    if (updates.tradeNo) tradeNo.value = updates.tradeNo
    if (updates.paymentNo) paymentNo.value = updates.paymentNo
    if (updates.qrCodeUrl) {
      // 保存二维码URL到localStorage
      localStorage.setItem(`qr_code_${orderId.value}`, updates.qrCodeUrl)
    }

    // Update session storage
    sessionStorage.setItem('order', JSON.stringify({
      orderId: orderId.value,
      productName: productName.value,
      amount: amount.value,
      description: description.value,
      status: status.value,
      createdAt: createdAt.value,
      tradeNo: tradeNo.value,
      paymentNo: paymentNo.value,
      userId: userId.value
    }))
  }

  function getQrCodeUrl(orderNo) {
    return localStorage.getItem(`qr_code_${orderNo}`)
  }

  function loadFromSession() {
    const saved = sessionStorage.getItem('order')
    if (saved) {
      const data = JSON.parse(saved)
      orderId.value = data.orderId
      productName.value = data.productName
      amount.value = data.amount
      description.value = data.description
      status.value = data.status
      createdAt.value = data.createdAt
      tradeNo.value = data.tradeNo || ''
      paymentNo.value = data.paymentNo || ''
      userId.value = data.userId || null  // 加载userId
    }
  }

  function clear() {
    orderId.value = ''
    productName.value = ''
    amount.value = 0
    description.value = ''
    status.value = ''
    createdAt.value = ''
    tradeNo.value = ''
    paymentNo.value = ''
    userId.value = null  // 清除userId
    sessionStorage.removeItem('order')
  }

  function setOrders(ordersData) {
    orders.value = ordersData
  }

  function updateOrderInList(updatedOrder) {
    const index = orders.value.findIndex(o => o.order_no === updatedOrder.order_no)
    if (index !== -1) {
      orders.value[index] = updatedOrder
    }
  }

  function setOrderFilter(filter) {
    orderFilter.value = filter
  }

  function setRefundInfo(info) {
    refundInfo.value = info
  }

  function clearRefundInfo() {
    refundInfo.value = null
  }

  function setCurrentOrder(order) {
    currentOrder.value = order
  }

  function clearCurrentOrder() {
    currentOrder.value = null
  }

  return {
    orderId,
    productName,
    amount,
    description,
    status,
    createdAt,
    tradeNo,
    paymentNo,
    userId,  // 导出userId
    orders,
    orderFilter,
    refundInfo,
    currentOrder,
    setOrder,
    updateOrder,
    loadFromSession,
    clear,
    setOrders,
    updateOrderInList,
    setOrderFilter,
    setRefundInfo,
    clearRefundInfo,
    setCurrentOrder,
    clearCurrentOrder,
    getQrCodeUrl  // 导出getQrCodeUrl
  }
})